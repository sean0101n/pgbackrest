/***********************************************************************************************************************************
Test Backup Command
***********************************************************************************************************************************/
#include <utime.h>

#include "command/stanza/create.h"
#include "command/stanza/upgrade.h"
#include "common/crypto/hash.h"
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "common/io/io.h"
#include "postgres/interface/static.vendor.h"
#include "storage/helper.h"
#include "storage/posix/storage.h"

#include "common/harnessConfig.h"
#include "common/harnessPq.h"

/***********************************************************************************************************************************
Get a list of all files in the backup and a redacted version of the manifest that can be tested against a static string
***********************************************************************************************************************************/
typedef struct TestBackupValidateCallbackData
{
    const Storage *storage;                                         // Storage object when needed (e.g. fileCompressed = true)
    const String *path;                                             // Subpath when storage is specified
    const Manifest *manifest;                                       // Manifest to check for files/links/paths
    const ManifestData *manifestData;                               // Manifest data
    String *content;                                                // String where content should be added
} TestBackupValidateCallbackData;

void
testBackupValidateCallback(void *callbackData, const StorageInfo *info)
{
    TestBackupValidateCallbackData *data = callbackData;

    // Don't include . when it is a path (we'll still include it when it is a link so we can see the destination)
    if (info->type == storageTypePath && strEq(info->name, DOT_STR))
        return;

    // Don't include backup.manifest or copy.  We'll test that they are present elsewhere
    if (info->type == storageTypeFile &&
        (strEqZ(info->name, BACKUP_MANIFEST_FILE) || strEqZ(info->name, BACKUP_MANIFEST_FILE INFO_COPY_EXT)))
        return;

    // Get manifest name
    const String *manifestName = info->name;

    strCatFmt(data->content, "%s {", strPtr(info->name));

    switch (info->type)
    {
        case storageTypeFile:
        {
            strCat(data->content, "file");

            // Calculate checksum/size and decompress if needed
            // ---------------------------------------------------------------------------------------------------------------------
            StorageRead *read = storageNewReadP(
                data->storage,
                data->path != NULL ? strNewFmt("%s/%s", strPtr(data->path), strPtr(info->name)) : info->name);

            if (data->manifestData->backupOptionCompressType != compressTypeNone)
            {
                ioFilterGroupAdd(
                    ioReadFilterGroup(storageReadIo(read)), decompressFilter(data->manifestData->backupOptionCompressType));
                manifestName = strSubN(
                    info->name, 0, strSize(info->name) - strSize(compressExtStr(data->manifestData->backupOptionCompressType)));
            }

            ioFilterGroupAdd(ioReadFilterGroup(storageReadIo(read)), cryptoHashNew(HASH_TYPE_SHA1_STR));

            uint64_t size = bufUsed(storageGetP(read));
            const String *checksum = varStr(
                ioFilterGroupResult(ioReadFilterGroup(storageReadIo(read)), CRYPTO_HASH_FILTER_TYPE_STR));

            strCatFmt(data->content, ", s=%" PRIu64, size);

            // Check against the manifest
            // ---------------------------------------------------------------------------------------------------------------------
            const ManifestFile *file = manifestFileFind(data->manifest, manifestName);

            // Test size and repo-size. If compressed then set the repo-size to size so it will not be in test output. Even the same
            // compression algorithm can give slightly different results based on the version so repo-size is not deterministic for
            // compression.
            if (size != file->size)
                THROW_FMT(AssertError, "'%s' size does match manifest", strPtr(manifestName));

            if (info->size != file->sizeRepo)
            {
                THROW_FMT(AssertError, "'%s' repo size does match manifest", strPtr(manifestName));
            }

            if (data->manifestData->backupOptionCompressType != compressTypeNone)
                ((ManifestFile *)file)->sizeRepo = file->size;

            // Test the checksum. pg_control and WAL headers have different checksums depending on cpu architecture so remove
            // the checksum from the test output.
            if (!strEqZ(checksum, file->checksumSha1))
            {
                THROW_FMT(AssertError, "'%s' checksum does match manifest", strPtr(manifestName));
            }

            if (strEqZ(manifestName, MANIFEST_TARGET_PGDATA "/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL) ||
                strBeginsWith(
                    manifestName, strNewFmt(MANIFEST_TARGET_PGDATA "/%s/", strPtr(pgWalPath(data->manifestData->pgVersion)))))
            {
                ((ManifestFile *)file)->checksumSha1[0] = '\0';
            }

            // Test mode, user, group. These values are not in the manifest but we know what they should be based on the default
            // mode and current user/group.
            if (info->mode != 0640)
                THROW_FMT(AssertError, "'%s' mode is not 0640", strPtr(manifestName));

            if (!strEqZ(info->user, testUser()))
                THROW_FMT(AssertError, "'%s' user should be '%s'", strPtr(manifestName), testUser());

            if (!strEqZ(info->group, testGroup()))
                THROW_FMT(AssertError, "'%s' group should be '%s'", strPtr(manifestName), testGroup());

            break;
        }

        case storageTypeLink:
        {
            strCatFmt(data->content, "link, d=%s", strPtr(info->linkDestination));
            break;
        }

        case storageTypePath:
        {
            strCat(data->content, "path");

            // Check against the manifest
            // ---------------------------------------------------------------------------------------------------------------------
            manifestPathFind(data->manifest, info->name);

            // Test mode, user, group. These values are not in the manifest but we know what they should be based on the default
            // mode and current user/group.
            if (info->mode != 0750)
                THROW_FMT(AssertError, "'%s' mode is not 00750", strPtr(info->name));

            if (!strEqZ(info->user, testUser()))
                THROW_FMT(AssertError, "'%s' user should be '%s'", strPtr(info->name), testUser());

            if (!strEqZ(info->group, testGroup()))
                THROW_FMT(AssertError, "'%s' group should be '%s'", strPtr(info->name), testGroup());

            break;
        }

        case storageTypeSpecial:
        {
            THROW_FMT(AssertError, "unexpected special file '%s'", strPtr(info->name));
            break;
        }
    }

    strCat(data->content, "}\n");
}

static String *
testBackupValidate(const Storage *storage, const String *path)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(STORAGE, storage);
        FUNCTION_HARNESS_PARAM(STRING, path);
    FUNCTION_HARNESS_END();

    String *result = strNew("");

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Build a list of files in the backup path and verify against the manifest
        // -------------------------------------------------------------------------------------------------------------------------
        Manifest *manifest = manifestLoadFile(storage, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strPtr(path)), cipherTypeNone, NULL);

        TestBackupValidateCallbackData callbackData =
        {
            .storage = storage,
            .path = path,
            .content = result,
            .manifest = manifest,
            .manifestData = manifestData(manifest),
        };

        storageInfoListP(storage, path, testBackupValidateCallback, &callbackData, .recurse = true, .sortOrder = sortOrderAsc);

        // Make sure both backup.manifest files exist since we skipped them in the callback above
        if (!storageExistsP(storage, strNewFmt("%s/" BACKUP_MANIFEST_FILE, strPtr(path))))
            THROW(AssertError, BACKUP_MANIFEST_FILE " is missing");

        if (!storageExistsP(storage, strNewFmt("%s/" BACKUP_MANIFEST_FILE INFO_COPY_EXT, strPtr(path))))
            THROW(AssertError, BACKUP_MANIFEST_FILE INFO_COPY_EXT " is missing");

        // Output the manifest to a string and exclude sections that don't need validation. Note that each of these sections should
        // be considered from automatic validation but adding them to the output will make the tests too noisy. One good technique
        // would be to remove it from the output only after validation so new values will cause changes in the output.
        // -------------------------------------------------------------------------------------------------------------------------
        Buffer *manifestSaveBuffer = bufNew(0);
        manifestSave(manifest, ioBufferWriteNew(manifestSaveBuffer));

        String *manifestEdit = strNew("");
        StringList *manifestLine = strLstNewSplitZ(strTrim(strNewBuf(manifestSaveBuffer)), "\n");
        bool bSkipSection = false;

        for (unsigned int lineIdx = 0; lineIdx < strLstSize(manifestLine); lineIdx++)
        {
            const String *line = strTrim(strLstGet(manifestLine, lineIdx));

            if (strChr(line, '[') == 0)
            {
                const String *section = strSubN(line, 1, strSize(line) - 2);

                if (strEq(section, INFO_SECTION_BACKREST_STR) ||
                    strEq(section, MANIFEST_SECTION_BACKUP_STR) ||
                    strEq(section, MANIFEST_SECTION_BACKUP_DB_STR) ||
                    strEq(section, MANIFEST_SECTION_BACKUP_OPTION_STR) ||
                    strEq(section, MANIFEST_SECTION_DB_STR) ||
                    strEq(section, MANIFEST_SECTION_TARGET_FILE_DEFAULT_STR) ||
                    strEq(section, MANIFEST_SECTION_TARGET_LINK_DEFAULT_STR) ||
                    strEq(section, MANIFEST_SECTION_TARGET_PATH_DEFAULT_STR))
                {
                    bSkipSection = true;
                }
                else
                    bSkipSection = false;
            }

            if (!bSkipSection)
                strCatFmt(manifestEdit, "%s\n", strPtr(line));
        }

        strCatFmt(result, "--------\n%s\n", strPtr(strTrim(manifestEdit)));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_HARNESS_RESULT(STRING, result);
}

/***********************************************************************************************************************************
Generate pq scripts for versions of PostgreSQL
***********************************************************************************************************************************/
typedef struct TestBackupPqScriptParam
{
    VAR_PARAM_HEADER;
    bool startFast;
    bool backupStandby;
    bool errorAfterStart;
    bool noWal;                                                     // Don't write test WAL segments
    CompressType walCompressType;                                   // Compress type for the archive files
    unsigned int walTotal;                                          // Total WAL to write
    unsigned int timeline;                                          // Timeline to use for WAL files
} TestBackupPqScriptParam;

#define testBackupPqScriptP(pgVersion, backupStartTime, ...)                                                                                           \
    testBackupPqScript(pgVersion, backupStartTime, (TestBackupPqScriptParam){VAR_PARAM_INIT, __VA_ARGS__})

static void
testBackupPqScript(unsigned int pgVersion, time_t backupTimeStart, TestBackupPqScriptParam param)
{
    const char *pg1Path = strPtr(strNewFmt("%s/pg1", testPath()));
    const char *pg2Path = strPtr(strNewFmt("%s/pg2", testPath()));

    // If no timeline specified then use timeline 1
    param.timeline = param.timeline == 0 ? 1 : param.timeline;

    // Read pg_control to get info about the cluster
    PgControl pgControl = pgControlFromFile(storagePg());

    // Set archive timeout really small to save time on errors
    cfgOptionSet(cfgOptArchiveTimeout, cfgSourceParam, varNewDbl(.1));

    uint64_t lsnStart = ((uint64_t)backupTimeStart & 0xFFFFFF00) << 28;
    uint64_t lsnStop =
        lsnStart + ((param.walTotal == 0 ? 0 : param.walTotal - 1) * pgControl.walSegmentSize) + (pgControl.walSegmentSize / 2);

    const char *lsnStartStr = strPtr(pgLsnToStr(lsnStart));
    const char *walSegmentStart = strPtr(pgLsnToWalSegment(param.timeline, lsnStart, pgControl.walSegmentSize));
    const char *lsnStopStr = strPtr(pgLsnToStr(lsnStop));
    const char *walSegmentStop = strPtr(pgLsnToWalSegment(param.timeline, lsnStop, pgControl.walSegmentSize));

    // Write WAL segments to the archive
    // -----------------------------------------------------------------------------------------------------------------------------
    if (!param.noWal)
    {
        InfoArchive *infoArchive = infoArchiveLoadFile(storageRepo(), INFO_ARCHIVE_PATH_FILE_STR, cipherTypeNone, NULL);
        const String *archiveId = infoArchiveId(infoArchive);
        StringList *walSegmentList = pgLsnRangeToWalSegmentList(
            pgControl.version, param.timeline, lsnStart, lsnStop, pgControl.walSegmentSize);

        Buffer *walBuffer = bufNew((size_t)pgControl.walSegmentSize);
        bufUsedSet(walBuffer, bufSize(walBuffer));
        memset(bufPtr(walBuffer), 0, bufSize(walBuffer));
        pgWalTestToBuffer((PgWal){.version = pgControl.version, .systemId = pgControl.systemId}, walBuffer);
        const String *walChecksum = bufHex(cryptoHashOne(HASH_TYPE_SHA1_STR, walBuffer));

        for (unsigned int walSegmentIdx = 0; walSegmentIdx < strLstSize(walSegmentList); walSegmentIdx++)
        {
            StorageWrite *write = storageNewWriteP(
                storageRepoWrite(),
                strNewFmt(
                    STORAGE_REPO_ARCHIVE "/%s/%s-%s%s", strPtr(archiveId), strPtr(strLstGet(walSegmentList, walSegmentIdx)),
                    strPtr(walChecksum), strPtr(compressExtStr(param.walCompressType))));

            if (param.walCompressType != compressTypeNone)
                ioFilterGroupAdd(ioWriteFilterGroup(storageWriteIo(write)), compressFilter(param.walCompressType, 1));

            storagePutP(write, walBuffer);
        }
    }

    // -----------------------------------------------------------------------------------------------------------------------------
    if (pgVersion == PG_VERSION_95)
    {
        ASSERT(!param.backupStandby);
        ASSERT(!param.errorAfterStart);

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_95, pg1Path, false, NULL, NULL),

            // Get start time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000),

            // Start backup
            HRNPQ_MACRO_ADVISORY_LOCK(1, true),
            HRNPQ_MACRO_IS_IN_BACKUP(1, false),
            HRNPQ_MACRO_START_BACKUP_84_95(1, param.startFast, lsnStartStr, walSegmentStart),
            HRNPQ_MACRO_DATABASE_LIST_1(1, "test1"),
            HRNPQ_MACRO_TABLESPACE_LIST_0(1),

            // Get copy start time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 999),
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 1000),

            // Stop backup
            HRNPQ_MACRO_STOP_BACKUP_LE_95(1, lsnStopStr, walSegmentStop),

            // Get stop time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 2000),

            HRNPQ_MACRO_DONE()
        });
    }
    // -----------------------------------------------------------------------------------------------------------------------------
    else if (pgVersion == PG_VERSION_96)
    {
        ASSERT(param.backupStandby);
        ASSERT(!param.errorAfterStart);

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_96, pg1Path, false, NULL, NULL),

            // Connect to standby
            HRNPQ_MACRO_OPEN_GE_92(2, "dbname='postgres' port=5433", PG_VERSION_96, pg2Path, true, NULL, NULL),

            // Get start time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000),

            // Start backup
            HRNPQ_MACRO_ADVISORY_LOCK(1, true),
            HRNPQ_MACRO_START_BACKUP_96(1, true, lsnStartStr, walSegmentStart),
            HRNPQ_MACRO_DATABASE_LIST_1(1, "test1"),
            HRNPQ_MACRO_TABLESPACE_LIST_0(1),

            // Wait for standby to sync
            HRNPQ_MACRO_REPLAY_WAIT_96(2, lsnStartStr),

            // Get copy start time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 999),
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 1000),

            // Stop backup
            HRNPQ_MACRO_STOP_BACKUP_96(1, lsnStopStr, walSegmentStop, false),

            // Get stop time
            HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 2000),

            HRNPQ_MACRO_DONE()
        });
    }
    // -----------------------------------------------------------------------------------------------------------------------------
    else if (pgVersion == PG_VERSION_11)
    {
        ASSERT(!param.backupStandby);

        if (param.errorAfterStart)
        {
            harnessPqScriptSet((HarnessPq [])
            {
                // Connect to primary
                HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_11, pg1Path, false, NULL, NULL),

                // Get start time
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000),

                // Start backup
                HRNPQ_MACRO_ADVISORY_LOCK(1, true),
                HRNPQ_MACRO_START_BACKUP_GE_10(1, param.startFast, lsnStartStr, walSegmentStart),
                HRNPQ_MACRO_DATABASE_LIST_1(1, "test1"),
                HRNPQ_MACRO_TABLESPACE_LIST_1(1, 32768, "tblspc32768"),

                // Get copy start time
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 999),
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 1000),

                HRNPQ_MACRO_DONE()
            });
        }
        else
        {
            harnessPqScriptSet((HarnessPq [])
            {
                // Connect to primary
                HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_11, pg1Path, false, NULL, NULL),

                // Get start time
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000),

                // Start backup
                HRNPQ_MACRO_ADVISORY_LOCK(1, true),
                HRNPQ_MACRO_START_BACKUP_GE_10(1, param.startFast, lsnStartStr, walSegmentStart),
                HRNPQ_MACRO_DATABASE_LIST_1(1, "test1"),
                HRNPQ_MACRO_TABLESPACE_LIST_1(1, 32768, "tblspc32768"),

                // Get copy start time
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 999),
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 1000),

                // Stop backup
                HRNPQ_MACRO_STOP_BACKUP_GE_10(1, lsnStopStr, walSegmentStop, false),

                // Get stop time
                HRNPQ_MACRO_TIME_QUERY(1, (int64_t)backupTimeStart * 1000 + 2000),

                HRNPQ_MACRO_DONE()
            });
        }
    }
    else
        THROW_FMT(AssertError, "unsupported test version %u", pgVersion);           // {uncoverable - no invalid versions in tests}
};

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    // The tests expect the timezone to be UTC
    setenv("TZ", "UTC", true);

    Storage *storageTest = storagePosixNewP(strNew(testPath()), .write = true);

    // Start a protocol server to test the protocol directly
    Buffer *serverWrite = bufNew(8192);
    IoWrite *serverWriteIo = ioBufferWriteNew(serverWrite);
    ioWriteOpen(serverWriteIo);

    ProtocolServer *server = protocolServerNew(strNew("test"), strNew("test"), ioBufferReadNew(bufNew(0)), serverWriteIo);
    bufUsedSet(serverWrite, 0);

    const String *pgFile = strNew("testfile");
    const String *missingFile = strNew("missing");
    const String *backupLabel = strNew("20190718-155825F");
    const String *backupPathFile = strNewFmt(STORAGE_REPO_BACKUP "/%s/%s", strPtr(backupLabel), strPtr(pgFile));
    BackupFileResult result = {0};
    VariantList *paramList = varLstNew();

    // *****************************************************************************************************************************
    if (testBegin("segmentNumber()"))
    {
        TEST_RESULT_UINT(segmentNumber(pgFile), 0, "No segment number");
        TEST_RESULT_UINT(segmentNumber(strNewFmt("%s.123", strPtr(pgFile))), 123, "Segment number");
    }

    // *****************************************************************************************************************************
    if (testBegin("backupFile(), backupProtocol"))
    {
        // Load Parameters
        StringList *argList = strLstNew();
        strLstAddZ(argList, "--stanza=test1");
        strLstAdd(argList, strNewFmt("--repo1-path=%s/repo", testPath()));
        strLstAdd(argList, strNewFmt("--pg1-path=%s/pg", testPath()));
        strLstAddZ(argList, "--repo1-retention-full=1");
        harnessCfgLoad(cfgCmdBackup, argList);

        // Create the pg path
        storagePathCreateP(storagePgWrite(), NULL, .mode = 0700);

        // Pg file missing - ignoreMissing=true
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ASSIGN(
            result,
            backupFile(
                missingFile, true, 0, true, NULL, false, 0, missingFile, false, compressTypeNone, 1, backupLabel, false,
                cipherTypeNone, NULL),
            "pg file missing, ignoreMissing=true, no delta");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 0, "    copy/repo size 0");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultSkip, "    skip file");

        // Check protocol function directly
        // -------------------------------------------------------------------------------------------------------------------------
        // NULL, zero param values, ignoreMissing=true
        varLstAdd(paramList, varNewStr(missingFile));       // pgFile
        varLstAdd(paramList, varNewBool(true));             // pgFileIgnoreMissing
        varLstAdd(paramList, varNewUInt64(0));              // pgFileSize
        varLstAdd(paramList, varNewBool(true));             // pgFileCopyExactSize
        varLstAdd(paramList, NULL);                         // pgFileChecksum
        varLstAdd(paramList, varNewBool(false));            // pgFileChecksumPage
        varLstAdd(paramList, varNewUInt64(0));              // pgFileChecksumPageLsnLimit
        varLstAdd(paramList, varNewStr(missingFile));       // repoFile
        varLstAdd(paramList, varNewBool(false));            // repoFileHasReference
        varLstAdd(paramList, varNewUInt(compressTypeNone)); // repoFileCompress
        varLstAdd(paramList, varNewInt(0));                 // repoFileCompressLevel
        varLstAdd(paramList, varNewStr(backupLabel));       // backupLabel
        varLstAdd(paramList, varNewBool(false));            // delta
        varLstAdd(paramList, NULL);                         // cipherSubPass

        TEST_RESULT_BOOL(
            backupProtocol(PROTOCOL_COMMAND_BACKUP_FILE_STR, paramList, server), true, "protocol backup file - skip");
        TEST_RESULT_STR_Z(strNewBuf(serverWrite), "{\"out\":[3,0,0,null,null]}\n", "    check result");
        bufUsedSet(serverWrite, 0);

        // Pg file missing - ignoreMissing=false
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_ERROR_FMT(
            backupFile(
                missingFile, false, 0, true, NULL, false, 0, missingFile, false, compressTypeNone, 1, backupLabel, false,
                cipherTypeNone, NULL),
            FileMissingError, "unable to open missing file '%s/pg/missing' for read", testPath());

        // Create a pg file to backup
        storagePutP(storageNewWriteP(storagePgWrite(), pgFile), BUFSTRDEF("atestfile"));

        // -------------------------------------------------------------------------------------------------------------------------
        // No prior checksum, no compression, no pageChecksum, no delta, no hasReference

        // With the expected backupCopyResultCopy, unset the storageFeatureCompress bit for the storageRepo for code coverage
        uint64_t feature = storageRepo()->interface.feature;
        ((Storage *)storageRepo())->interface.feature = feature && ((1 << storageFeatureCompress) ^ 0xFFFFFFFFFFFFFFFF);

        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9999999, true, NULL, false, 0, pgFile, false, compressTypeNone, 1, backupLabel, false,
                cipherTypeNone, NULL),
            "pg file exists and shrunk, no repo file, no ignoreMissing, no pageChecksum, no delta, no hasReference");

        ((Storage *)storageRepo())->interface.feature = feature;

        TEST_RESULT_UINT(result.copySize + result.repoSize, 18, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    copy file to repo success");

        TEST_RESULT_VOID(storageRemoveP(storageRepoWrite(), backupPathFile), "    remove repo file");

        // -------------------------------------------------------------------------------------------------------------------------
        // Test pagechecksum

        // Increase the file size but most of the following tests will still treat the file as size 9.  This tests the common case
        // where a file grows while a backup is running.
        storagePutP(storageNewWriteP(storagePgWrite(), pgFile), BUFSTRDEF("atestfile###"));

        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, NULL, true, 0xFFFFFFFFFFFFFFFF, pgFile, false, compressTypeNone, 1, backupLabel, false,
                cipherTypeNone, NULL),
            "file checksummed with pageChecksum enabled");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 18, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile)),
            true,"    copy file to repo success");
        TEST_RESULT_PTR_NE(result.pageChecksumResult, NULL, "    pageChecksumResult is set");
        TEST_RESULT_BOOL(
            varBool(kvGet(result.pageChecksumResult, VARSTRDEF("valid"))), false, "    pageChecksumResult valid=false");
        TEST_RESULT_VOID(storageRemoveP(storageRepoWrite(), backupPathFile), "    remove repo file");

        // Check protocol function directly
        // -------------------------------------------------------------------------------------------------------------------------
        // pgFileSize, ignoreMissing=false, backupLabel, pgFileChecksumPage, pgFileChecksumPageLsnLimit
        paramList = varLstNew();
        varLstAdd(paramList, varNewStr(pgFile));            // pgFile
        varLstAdd(paramList, varNewBool(false));            // pgFileIgnoreMissing
        varLstAdd(paramList, varNewUInt64(8));              // pgFileSize
        varLstAdd(paramList, varNewBool(false));            // pgFileCopyExactSize
        varLstAdd(paramList, NULL);                         // pgFileChecksum
        varLstAdd(paramList, varNewBool(true));             // pgFileChecksumPage
        varLstAdd(paramList, varNewUInt64(0xFFFFFFFFFFFFFFFF)); // pgFileChecksumPageLsnLimit
        varLstAdd(paramList, varNewStr(pgFile));            // repoFile
        varLstAdd(paramList, varNewBool(false));            // repoFileHasReference
        varLstAdd(paramList, varNewUInt(compressTypeNone)); // repoFileCompress
        varLstAdd(paramList, varNewInt(1));                 // repoFileCompressLevel
        varLstAdd(paramList, varNewStr(backupLabel));       // backupLabel
        varLstAdd(paramList, varNewBool(false));            // delta
        varLstAdd(paramList, NULL);                         // cipherSubPass

        TEST_RESULT_BOOL(
            backupProtocol(PROTOCOL_COMMAND_BACKUP_FILE_STR, paramList, server), true, "protocol backup file - pageChecksum");
        TEST_RESULT_STR_Z(
            strNewBuf(serverWrite),
            "{\"out\":[1,12,12,\"c3ae4687ea8ccd47bfdb190dbe7fd3b37545fdb9\",{\"align\":false,\"valid\":false}]}\n",
            "    check result");
        bufUsedSet(serverWrite, 0);

        // -------------------------------------------------------------------------------------------------------------------------
        // File exists in repo and db, checksum match, delta set, ignoreMissing false, hasReference - NOOP
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, true,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "file in db and repo, checksum equal, no ignoreMissing, no pageChecksum, delta, hasReference");
        TEST_RESULT_UINT(result.copySize, 9, "    copy size set");
        TEST_RESULT_UINT(result.repoSize, 0, "    repo size not set since already exists in repo");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultNoOp, "    noop file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    noop");

        // Check protocol function directly
        // -------------------------------------------------------------------------------------------------------------------------
        // pgFileChecksum, hasReference, delta
        paramList = varLstNew();
        varLstAdd(paramList, varNewStr(pgFile));            // pgFile
        varLstAdd(paramList, varNewBool(false));            // pgFileIgnoreMissing
        varLstAdd(paramList, varNewUInt64(12));             // pgFileSize
        varLstAdd(paramList, varNewBool(false));            // pgFileCopyExactSize
        varLstAdd(paramList, varNewStrZ("c3ae4687ea8ccd47bfdb190dbe7fd3b37545fdb9"));   // pgFileChecksum
        varLstAdd(paramList, varNewBool(false));            // pgFileChecksumPage
        varLstAdd(paramList, varNewUInt64(0));              // pgFileChecksumPageLsnLimit
        varLstAdd(paramList, varNewStr(pgFile));            // repoFile
        varLstAdd(paramList, varNewBool(true));             // repoFileHasReference
        varLstAdd(paramList, varNewUInt(compressTypeNone)); // repoFileCompress
        varLstAdd(paramList, varNewInt(1));                 // repoFileCompressLevel
        varLstAdd(paramList, varNewStr(backupLabel));       // backupLabel
        varLstAdd(paramList, varNewBool(true));             // delta
        varLstAdd(paramList, NULL);                         // cipherSubPass

        TEST_RESULT_BOOL(
            backupProtocol(PROTOCOL_COMMAND_BACKUP_FILE_STR, paramList, server), true, "protocol backup file - noop");
        TEST_RESULT_STR_Z(
            strNewBuf(serverWrite), "{\"out\":[4,12,0,\"c3ae4687ea8ccd47bfdb190dbe7fd3b37545fdb9\",null]}\n", "    check result");
        bufUsedSet(serverWrite, 0);

        // -------------------------------------------------------------------------------------------------------------------------
        // File exists in repo and db, pg checksum mismatch, delta set, ignoreMissing false, hasReference - COPY
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("1234567890123456789012345678901234567890"), false, 0, pgFile, true,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "file in db and repo, pg checksum not equal, no ignoreMissing, no pageChecksum, delta, hasReference");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 18, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    copy");

        // -------------------------------------------------------------------------------------------------------------------------
        // File exists in repo and db, pg checksum same, pg size different, delta set, ignoreMissing false, hasReference - COPY
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9999999, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, true,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "db & repo file, pg checksum same, pg size different, no ignoreMissing, no pageChecksum, delta, hasReference");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 24, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_STR_Z(result.copyChecksum, "c3ae4687ea8ccd47bfdb190dbe7fd3b37545fdb9", "TEST");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "c3ae4687ea8ccd47bfdb190dbe7fd3b37545fdb9") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    copy");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("resumed file is missing in repo but present in resumed manfest, recopy");

        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, STRDEF(BOGUS_STR), false,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "backup file");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 18, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultReCopy, "    check copy result");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    recopy");

        // -------------------------------------------------------------------------------------------------------------------------
        // File exists in repo and db, checksum not same in repo, delta set, ignoreMissing false, no hasReference - RECOPY
        TEST_RESULT_VOID(
            storagePutP(storageNewWriteP(storageRepoWrite(), backupPathFile), BUFSTRDEF("adifferentfile")),
            "create different file (size and checksum) with same name in repo");
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, false,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "    db & repo file, pgFileMatch, repo checksum no match, no ignoreMissing, no pageChecksum, delta, no hasReference");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 18, "    copy=repo=pgFile size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultReCopy, "    recopy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    recopy");

        // -------------------------------------------------------------------------------------------------------------------------
        // File exists in repo but missing from db, checksum same in repo, delta set, ignoreMissing true, no hasReference - SKIP
        TEST_RESULT_VOID(
            storagePutP(storageNewWriteP(storageRepoWrite(), backupPathFile), BUFSTRDEF("adifferentfile")),
            "create different file with same name in repo");
        TEST_ASSIGN(
            result,
            backupFile(
                missingFile, true, 9, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, false,
                compressTypeNone, 1, backupLabel, true, cipherTypeNone, NULL),
            "    file in repo only, checksum in repo equal, ignoreMissing=true, no pageChecksum, delta, no hasReference");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 0, "    copy=repo=0 size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultSkip, "    skip file");
        TEST_RESULT_BOOL(
            (result.copyChecksum == NULL && !storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    skip and remove file from repo");

        // -------------------------------------------------------------------------------------------------------------------------
        // No prior checksum, compression, no page checksum, no pageChecksum, no delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, NULL, false, 0, pgFile, false, compressTypeGz, 3, backupLabel, false, cipherTypeNone, NULL),
            "pg file exists, no checksum, no ignoreMissing, compression, no pageChecksum, no delta, no hasReference");

        TEST_RESULT_UINT(result.copySize, 9, "    copy=pgFile size");
        TEST_RESULT_UINT(result.repoSize, 29, "    repo compress size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), strNewFmt(STORAGE_REPO_BACKUP "/%s/%s.gz", strPtr(backupLabel), strPtr(pgFile))) &&
                result.pageChecksumResult == NULL),
            true, "    copy file to repo compress success");

        // -------------------------------------------------------------------------------------------------------------------------
        // Pg and repo file exist & match, prior checksum, compression, no page checksum, no pageChecksum, no delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, false, compressTypeGz,
                3, backupLabel, false, cipherTypeNone, NULL),
            "pg file & repo exists, match, checksum, no ignoreMissing, compression, no pageChecksum, no delta, no hasReference");

        TEST_RESULT_UINT(result.copySize, 9, "    copy=pgFile size");
        TEST_RESULT_UINT(result.repoSize, 29, "    repo compress size");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultChecksum, "    checksum file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), strNewFmt(STORAGE_REPO_BACKUP "/%s/%s.gz", strPtr(backupLabel), strPtr(pgFile))) &&
                result.pageChecksumResult == NULL),
            true, "    compressed repo file matches");

        // Check protocol function directly
        // -------------------------------------------------------------------------------------------------------------------------
        // compression
        paramList = varLstNew();
        varLstAdd(paramList, varNewStr(pgFile));            // pgFile
        varLstAdd(paramList, varNewBool(false));            // pgFileIgnoreMissing
        varLstAdd(paramList, varNewUInt64(9));              // pgFileSize
        varLstAdd(paramList, varNewBool(true));             // pgFileCopyExactSize
        varLstAdd(paramList, varNewStrZ("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"));   // pgFileChecksum
        varLstAdd(paramList, varNewBool(false));            // pgFileChecksumPage
        varLstAdd(paramList, varNewUInt64(0));              // pgFileChecksumPageLsnLimit
        varLstAdd(paramList, varNewStr(pgFile));            // repoFile
        varLstAdd(paramList, varNewBool(false));            // repoFileHasReference
        varLstAdd(paramList, varNewUInt(compressTypeGz));   // repoFileCompress
        varLstAdd(paramList, varNewInt(3));                 // repoFileCompressLevel
        varLstAdd(paramList, varNewStr(backupLabel));       // backupLabel
        varLstAdd(paramList, varNewBool(false));            // delta
        varLstAdd(paramList, NULL);                         // cipherSubPass

        TEST_RESULT_BOOL(
            backupProtocol(PROTOCOL_COMMAND_BACKUP_FILE_STR, paramList, server), true, "protocol backup file - copy, compress");
        TEST_RESULT_STR_Z(
            strNewBuf(serverWrite), "{\"out\":[0,9,29,\"9bc8ab2dda60ef4beed07d1e19ce0676d5edde67\",null]}\n", "    check result");
        bufUsedSet(serverWrite, 0);

        // -------------------------------------------------------------------------------------------------------------------------
        // Create a zero sized file - checksum will be set but in backupManifestUpdate it will not be copied
        storagePutP(storageNewWriteP(storagePgWrite(), strNew("zerofile")), BUFSTRDEF(""));

        // No prior checksum, no compression, no pageChecksum, no delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                strNew("zerofile"), false, 0, true, NULL, false, 0, strNew("zerofile"), false, compressTypeNone, 1, backupLabel,
                false, cipherTypeNone, NULL),
            "zero-sized pg file exists, no repo file, no ignoreMissing, no pageChecksum, no delta, no hasReference");
        TEST_RESULT_UINT(result.copySize + result.repoSize, 0, "    copy=repo=pgFile size 0");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_PTR_NE(result.copyChecksum, NULL, "    checksum set");
        TEST_RESULT_BOOL(
            (storageExistsP(storageRepo(), strNewFmt(STORAGE_REPO_BACKUP "/%s/zerofile", strPtr(backupLabel))) &&
                result.pageChecksumResult == NULL),
            true, "    copy zero file to repo success");

        // Check invalid protocol function
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_RESULT_BOOL(backupProtocol(strNew(BOGUS_STR), paramList, server), false, "invalid function");
    }

    // *****************************************************************************************************************************
    if (testBegin("backupFile() - encrypt"))
    {
        // Load Parameters
        StringList *argList = strLstNew();
        strLstAddZ(argList, "--stanza=test1");
        strLstAdd(argList, strNewFmt("--repo1-path=%s/repo", testPath()));
        strLstAdd(argList, strNewFmt("--pg1-path=%s/pg", testPath()));
        strLstAddZ(argList, "--repo1-retention-full=1");
        strLstAddZ(argList, "--repo1-cipher-type=aes-256-cbc");
        setenv("PGBACKREST_REPO1_CIPHER_PASS", "12345678", true);
        harnessCfgLoad(cfgCmdBackup, argList);
        unsetenv("PGBACKREST_REPO1_CIPHER_PASS");

        // Create the pg path
        storagePathCreateP(storagePgWrite(), NULL, .mode = 0700);

        // Create a pg file to backup
        storagePutP(storageNewWriteP(storagePgWrite(), pgFile), BUFSTRDEF("atestfile"));

        // -------------------------------------------------------------------------------------------------------------------------
        // No prior checksum, no compression, no pageChecksum, no delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, NULL, false, 0, pgFile, false, compressTypeNone, 1, backupLabel, false, cipherTypeAes256Cbc,
                strNew("12345678")),
            "pg file exists, no repo file, no ignoreMissing, no pageChecksum, no delta, no hasReference");

        TEST_RESULT_UINT(result.copySize, 9, "    copy size set");
        TEST_RESULT_UINT(result.repoSize, 32, "    repo size set");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
            storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    copy file to encrypted repo success");

        // -------------------------------------------------------------------------------------------------------------------------
        // Delta but pgMatch false (pg File size different), prior checksum, no compression, no pageChecksum, delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 8, true, strNew("9bc8ab2dda60ef4beed07d1e19ce0676d5edde67"), false, 0, pgFile, false,
                compressTypeNone, 1, backupLabel, true, cipherTypeAes256Cbc, strNew("12345678")),
            "pg and repo file exists, pgFileMatch false, no ignoreMissing, no pageChecksum, delta, no hasReference");
        TEST_RESULT_UINT(result.copySize, 8, "    copy size set");
        TEST_RESULT_UINT(result.repoSize, 32, "    repo size set");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultCopy, "    copy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "acc972a8319d4903b839c64ec217faa3e77b4fcb") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    copy file (size missmatch) to encrypted repo success");

        // -------------------------------------------------------------------------------------------------------------------------
        // Check repo with cipher filter.
        // pg/repo file size same but checksum different, prior checksum, no compression, no pageChecksum, no delta, no hasReference
        TEST_ASSIGN(
            result,
            backupFile(
                pgFile, false, 9, true, strNew("1234567890123456789012345678901234567890"), false, 0, pgFile, false,
                compressTypeNone, 0, backupLabel, false, cipherTypeAes256Cbc, strNew("12345678")),
            "pg and repo file exists, repo checksum no match, no ignoreMissing, no pageChecksum, no delta, no hasReference");
        TEST_RESULT_UINT(result.copySize, 9, "    copy size set");
        TEST_RESULT_UINT(result.repoSize, 32, "    repo size set");
        TEST_RESULT_UINT(result.backupCopyResult, backupCopyResultReCopy, "    recopy file");
        TEST_RESULT_BOOL(
            (strEqZ(result.copyChecksum, "9bc8ab2dda60ef4beed07d1e19ce0676d5edde67") &&
                storageExistsP(storageRepo(), backupPathFile) && result.pageChecksumResult == NULL),
            true, "    recopy file to encrypted repo success");

        // Check protocol function directly
        // -------------------------------------------------------------------------------------------------------------------------
        // cipherType, cipherPass
        paramList = varLstNew();
        varLstAdd(paramList, varNewStr(pgFile));                // pgFile
        varLstAdd(paramList, varNewBool(false));                // pgFileIgnoreMissing
        varLstAdd(paramList, varNewUInt64(9));                  // pgFileSize
        varLstAdd(paramList, varNewBool(true));                 // pgFileCopyExactSize
        varLstAdd(paramList, varNewStrZ("1234567890123456789012345678901234567890"));   // pgFileChecksum
        varLstAdd(paramList, varNewBool(false));                // pgFileChecksumPage
        varLstAdd(paramList, varNewUInt64(0));                  // pgFileChecksumPageLsnLimit
        varLstAdd(paramList, varNewStr(pgFile));                // repoFile
        varLstAdd(paramList, varNewBool(false));                // repoFileHasReference
        varLstAdd(paramList, varNewUInt(compressTypeNone));     // repoFileCompress
        varLstAdd(paramList, varNewInt(0));                     // repoFileCompressLevel
        varLstAdd(paramList, varNewStr(backupLabel));           // backupLabel
        varLstAdd(paramList, varNewBool(false));                // delta
        varLstAdd(paramList, varNewStrZ("12345678"));           // cipherPass

        TEST_RESULT_BOOL(
            backupProtocol(PROTOCOL_COMMAND_BACKUP_FILE_STR, paramList, server), true, "protocol backup file - recopy, encrypt");
        TEST_RESULT_STR_Z(
            strNewBuf(serverWrite), "{\"out\":[2,9,32,\"9bc8ab2dda60ef4beed07d1e19ce0676d5edde67\",null]}\n", "    check result");
        bufUsedSet(serverWrite, 0);
    }

    // *****************************************************************************************************************************
    if (testBegin("backupLabelCreate()"))
    {
        const String *pg1Path = strNewFmt("%s/pg1", testPath());
        const String *repoPath = strNewFmt("%s/repo", testPath());

        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        harnessCfgLoad(cfgCmdBackup, argList);

        time_t timestamp = 1575401652;
        String *backupLabel = backupLabelFormat(backupTypeFull, NULL, timestamp);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("assign label when no history");

        storagePathCreateP(storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/backup.history/2019"));

        TEST_RESULT_STR(backupLabelCreate(backupTypeFull, NULL, timestamp), backupLabel, "create label");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("assign label when history is older");

        storagePutP(
            storageNewWriteP(
                storageRepoWrite(),
                strNewFmt(
                    STORAGE_REPO_BACKUP "/backup.history/2019/%s.manifest.gz",
                    strPtr(backupLabelFormat(backupTypeFull, NULL, timestamp - 4)))),
            NULL);

        TEST_RESULT_STR(backupLabelCreate(backupTypeFull, NULL, timestamp), backupLabel, "create label");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("assign label when backup is older");

        storagePutP(
            storageNewWriteP(
                storageRepoWrite(),
                strNewFmt(STORAGE_REPO_BACKUP "/%s", strPtr(backupLabelFormat(backupTypeFull, NULL, timestamp - 2)))),
            NULL);

        TEST_RESULT_STR(backupLabelCreate(backupTypeFull, NULL, timestamp), backupLabel, "create label");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("advance time when backup is same");

        storagePutP(
            storageNewWriteP(
                storageRepoWrite(),
                strNewFmt(STORAGE_REPO_BACKUP "/%s", strPtr(backupLabelFormat(backupTypeFull, NULL, timestamp)))),
            NULL);

        TEST_RESULT_STR_Z(backupLabelCreate(backupTypeFull, NULL, timestamp), "20191203-193413F", "create label");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when new label is in the past even with advanced time");

        storagePutP(
            storageNewWriteP(
                storageRepoWrite(),
                strNewFmt(STORAGE_REPO_BACKUP "/%s", strPtr(backupLabelFormat(backupTypeFull, NULL, timestamp + 1)))),
            NULL);

        TEST_ERROR(
            backupLabelCreate(backupTypeFull, NULL, timestamp), FormatError,
            "new backup label '20191203-193413F' is not later than latest backup label '20191203-193413F'\n"
            "HINT: has the timezone changed?\n"
            "HINT: is there clock skew?");
    }

    // *****************************************************************************************************************************
    if (testBegin("backupInit()"))
    {
        const String *pg1Path = strNewFmt("%s/pg1", testPath());
        const String *repoPath = strNewFmt("%s/repo", testPath());

        // Set log level to detail
        harnessLogLevelSet(logLevelDetail);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when backup from standby is not supported");

        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--" CFGOPT_BACKUP_STANDBY);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_ERROR(
            backupInit(infoBackupNew(PG_VERSION_91, 1000000000000000910, NULL)), ConfigError,
             "option 'backup-standby' not valid for PostgreSQL < 9.2");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("warn and reset when backup from standby used in offline mode");

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_92, .systemId = 1000000000000000920}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--" CFGOPT_BACKUP_STANDBY);
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_RESULT_VOID(backupInit(infoBackupNew(PG_VERSION_92, 1000000000000000920, NULL)), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptBackupStandby), false, "    check backup-standby");

        TEST_RESULT_LOG(
            "P00   WARN: option backup-standby is enabled but backup is offline - backups will be performed from the primary");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when pg_control does not match stanza");

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_10, .systemId = 1000000000000001000}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_ERROR(
            backupInit(infoBackupNew(PG_VERSION_11, 1000000000000001100, NULL)), BackupMismatchError,
            "PostgreSQL version 10, system-id 1000000000000001000 do not match stanza version 11, system-id 1000000000000001100\n"
            "HINT: is this the correct stanza?");
        TEST_ERROR(
            backupInit(infoBackupNew(PG_VERSION_10, 1000000000000001100, NULL)), BackupMismatchError,
            "PostgreSQL version 10, system-id 1000000000000001000 do not match stanza version 10, system-id 1000000000000001100\n"
            "HINT: is this the correct stanza?");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("reset start-fast when PostgreSQL < 8.4");

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_83, .systemId = 1000000000000000830}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--" CFGOPT_START_FAST);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_RESULT_VOID(backupInit(infoBackupNew(PG_VERSION_83, 1000000000000000830, NULL)), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptStartFast), false, "    check start-fast");

        TEST_RESULT_LOG("P00   WARN: start-fast option is only available in PostgreSQL >= 8.4");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("reset stop-auto when PostgreSQL < 9.3");

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_84, .systemId = 1000000000000000840}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--" CFGOPT_STOP_AUTO);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_RESULT_VOID(backupInit(infoBackupNew(PG_VERSION_84, 1000000000000000840, NULL)), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptStopAuto), false, "    check stop-auto");

        TEST_RESULT_LOG("P00   WARN: stop-auto option is only available in PostgreSQL >= 9.3");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("reset checksum-page when the cluster does not have checksums enabled");

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_93, .systemId = PG_VERSION_93}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--" CFGOPT_CHECKSUM_PAGE);
        harnessCfgLoad(cfgCmdBackup, argList);

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_96, strPtr(pg1Path), false, NULL, NULL),

            HRNPQ_MACRO_DONE()
        });

        TEST_RESULT_VOID(dbFree(backupInit(infoBackupNew(PG_VERSION_93, PG_VERSION_93, NULL))->dbPrimary), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptChecksumPage), false, "    check checksum-page");

        TEST_RESULT_LOG(
            "P00   WARN: checksum-page option set to true but checksums are not enabled on the cluster, resetting to false");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("ok if cluster checksums are enabled and checksum-page is any value");

        // Create pg_control with page checksums
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_93, .systemId = PG_VERSION_93, .pageChecksum = true}));

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_CHECKSUM_PAGE);
        harnessCfgLoad(cfgCmdBackup, argList);

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_96, strPtr(pg1Path), false, NULL, NULL),

            HRNPQ_MACRO_DONE()
        });

        TEST_RESULT_VOID(dbFree(backupInit(infoBackupNew(PG_VERSION_93, PG_VERSION_93, NULL))->dbPrimary), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptChecksumPage), false, "    check checksum-page");

        // Create pg_control without page checksums
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_93, .systemId = PG_VERSION_93}));

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_96, strPtr(pg1Path), false, NULL, NULL),

            HRNPQ_MACRO_DONE()
        });

        TEST_RESULT_VOID(dbFree(backupInit(infoBackupNew(PG_VERSION_93, PG_VERSION_93, NULL))->dbPrimary), "backup init");
        TEST_RESULT_BOOL(cfgOptionBool(cfgOptChecksumPage), false, "    check checksum-page");
    }

    // *****************************************************************************************************************************
    if (testBegin("backupTime()"))
    {
        const String *pg1Path = strNewFmt("%s/pg1", testPath());
        const String *repoPath = strNewFmt("%s/repo", testPath());

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when second does not advance after sleep");

        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        harnessCfgLoad(cfgCmdBackup, argList);

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_93, .systemId = PG_VERSION_93}));

        harnessPqScriptSet((HarnessPq [])
        {
            // Connect to primary
            HRNPQ_MACRO_OPEN_GE_92(1, "dbname='postgres' port=5432", PG_VERSION_96, strPtr(pg1Path), false, NULL, NULL),

            // Don't advance time after wait
            HRNPQ_MACRO_TIME_QUERY(1, 1575392588998),
            HRNPQ_MACRO_TIME_QUERY(1, 1575392588999),

            HRNPQ_MACRO_DONE()
        });

        BackupData *backupData = backupInit(infoBackupNew(PG_VERSION_93, PG_VERSION_93, NULL));

        TEST_ERROR(backupTime(backupData, true), AssertError, "invalid sleep for online backup time with wait remainder");
        dbFree(backupData->dbPrimary);
    }

    // *****************************************************************************************************************************
    if (testBegin("backupResumeFind()"))
    {
        const String *repoPath = strNewFmt("%s/repo", testPath());

        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAddZ(argList, "--" CFGOPT_PG1_PATH "=/pg");
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_FULL);
        strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
        harnessCfgLoad(cfgCmdBackup, argList);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume empty directory");

        storagePathCreateP(storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F"));

        TEST_RESULT_PTR(backupResumeFind((Manifest *)1, NULL), NULL, "find resumable backup");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume when resume is disabled");

        cfgOptionSet(cfgOptResume, cfgSourceParam, BOOL_FALSE_VAR);

        storagePutP(
            storageNewWriteP(
                storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F/" BACKUP_MANIFEST_FILE INFO_COPY_EXT)),
            NULL);

        TEST_RESULT_PTR(backupResumeFind((Manifest *)1, NULL), NULL, "find resumable backup");

        TEST_RESULT_LOG(
            "P00   WARN: backup '20191003-105320F' cannot be resumed: resume is disabled");

        TEST_RESULT_BOOL(
            storagePathExistsP(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F")), false, "check backup path removed");

        cfgOptionSet(cfgOptResume, cfgSourceParam, BOOL_TRUE_VAR);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume when pgBackRest version has changed");

        Manifest *manifestResume = manifestNewInternal();
        manifestResume->info = infoNew(NULL);
        manifestResume->data.backupType = backupTypeFull;
        manifestResume->data.backupLabel = STRDEF("20191003-105320F");
        manifestResume->data.pgVersion = PG_VERSION_12;

        manifestTargetAdd(manifestResume, &(ManifestTarget){.name = MANIFEST_TARGET_PGDATA_STR, .path = STRDEF("/pg")});
        manifestPathAdd(manifestResume, &(ManifestPath){.name = MANIFEST_TARGET_PGDATA_STR});
        manifestFileAdd(manifestResume, &(ManifestFile){.name = STRDEF("pg_data/" PG_FILE_PGVERSION)});

        manifestSave(
            manifestResume,
            storageWriteIo(
                storageNewWriteP(
                    storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F/" BACKUP_MANIFEST_FILE INFO_COPY_EXT))));

        Manifest *manifest = manifestNewInternal();
        manifest->data.backupType = backupTypeFull;
        manifest->data.backrestVersion = STRDEF("BOGUS");

        TEST_RESULT_PTR(backupResumeFind(manifest, NULL), NULL, "find resumable backup");

        TEST_RESULT_LOG(
            "P00   WARN: backup '20191003-105320F' cannot be resumed:"
                " new pgBackRest version 'BOGUS' does not match resumable pgBackRest version '" PROJECT_VERSION "'");

        TEST_RESULT_BOOL(
            storagePathExistsP(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F")), false, "check backup path removed");

        manifest->data.backrestVersion = STRDEF(PROJECT_VERSION);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume when backup labels do not match (resumable is null)");

        manifest->data.backupType = backupTypeFull;
        manifest->data.backupLabelPrior = STRDEF("20191003-105320F");

        manifestSave(
            manifestResume,
            storageWriteIo(
                storageNewWriteP(
                    storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F/" BACKUP_MANIFEST_FILE INFO_COPY_EXT))));

        TEST_RESULT_PTR(backupResumeFind(manifest, NULL), NULL, "find resumable backup");

        TEST_RESULT_LOG(
            "P00   WARN: backup '20191003-105320F' cannot be resumed:"
                " new prior backup label '<undef>' does not match resumable prior backup label '20191003-105320F'");

        TEST_RESULT_BOOL(
            storagePathExistsP(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F")), false, "check backup path removed");

        manifest->data.backupLabelPrior = NULL;

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume when backup labels do not match (new is null)");

        manifest->data.backupType = backupTypeFull;
        manifestResume->data.backupLabelPrior = STRDEF("20191003-105320F");

        manifestSave(
            manifestResume,
            storageWriteIo(
                storageNewWriteP(
                    storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F/" BACKUP_MANIFEST_FILE INFO_COPY_EXT))));

        TEST_RESULT_PTR(backupResumeFind(manifest, NULL), NULL, "find resumable backup");

        TEST_RESULT_LOG(
            "P00   WARN: backup '20191003-105320F' cannot be resumed:"
                " new prior backup label '20191003-105320F' does not match resumable prior backup label '<undef>'");

        TEST_RESULT_BOOL(
            storagePathExistsP(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F")), false, "check backup path removed");

        manifestResume->data.backupLabelPrior = NULL;

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("cannot resume when compression does not match");

        manifestResume->data.backupOptionCompressType = compressTypeGz;

        manifestSave(
            manifestResume,
            storageWriteIo(
                storageNewWriteP(
                    storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F/" BACKUP_MANIFEST_FILE INFO_COPY_EXT))));

        TEST_RESULT_PTR(backupResumeFind(manifest, NULL), NULL, "find resumable backup");

        TEST_RESULT_LOG(
            "P00   WARN: backup '20191003-105320F' cannot be resumed:"
                " new compression 'none' does not match resumable compression 'gz'");

        TEST_RESULT_BOOL(
            storagePathExistsP(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191003-105320F")), false, "check backup path removed");

        manifestResume->data.backupOptionCompressType = compressTypeNone;
    }

    // *****************************************************************************************************************************
    if (testBegin("backupJobResult()"))
    {
        // Set log level to detail
        harnessLogLevelSet(logLevelDetail);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("report job error");

        ProtocolParallelJob *job = protocolParallelJobNew(VARSTRDEF("key"), protocolCommandNew(STRDEF("command")));
        protocolParallelJobErrorSet(job, errorTypeCode(&AssertError), STRDEF("error message"));

        TEST_ERROR(backupJobResult((Manifest *)1, NULL, STRDEF("log"), strLstNew(), job, 0, 0), AssertError, "error message");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("report host/100% progress on noop result");

        // Create job that skips file
        job = protocolParallelJobNew(VARSTRDEF("pg_data/test"), protocolCommandNew(STRDEF("command")));

        VariantList *result = varLstNew();
        varLstAdd(result, varNewUInt64(backupCopyResultNoOp));
        varLstAdd(result, varNewUInt64(0));
        varLstAdd(result, varNewUInt64(0));
        varLstAdd(result, NULL);
        varLstAdd(result, NULL);

        protocolParallelJobResultSet(job, varNewVarLst(result));

        // Create manifest with file
        Manifest *manifest = manifestNewInternal();
        manifestFileAdd(manifest, &(ManifestFile){.name = STRDEF("pg_data/test")});

        TEST_RESULT_UINT(
            backupJobResult(manifest, STRDEF("host"), STRDEF("log-test"), strLstNew(), job, 0, 0), 0, "log noop result");

        TEST_RESULT_LOG("P00 DETAIL: match file from prior backup host:log-test (0B, 100%)");
    }

    // Offline tests should only be used to test offline functionality and errors easily tested in offline mode
    // *****************************************************************************************************************************
    if (testBegin("cmdBackup() offline"))
    {
        const String *pg1Path = strNewFmt("%s/pg1", testPath());
        const String *repoPath = strNewFmt("%s/repo", testPath());

        // Set log level to detail
        harnessLogLevelSet(logLevelDetail);

        // Replace backup labels since the times are not deterministic
        hrnLogReplaceAdd("[0-9]{8}-[0-9]{6}F_[0-9]{8}-[0-9]{6}I", NULL, "INCR", true);
        hrnLogReplaceAdd("[0-9]{8}-[0-9]{6}F_[0-9]{8}-[0-9]{6}D", NULL, "DIFF", true);
        hrnLogReplaceAdd("[0-9]{8}-[0-9]{6}F", NULL, "FULL", true);

        // Create pg_control
        storagePutP(
            storageNewWriteP(storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path))),
            pgControlTestToBuffer((PgControl){.version = PG_VERSION_84, .systemId = 1000000000000000840}));

        // Create stanza
        StringList *argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        harnessCfgLoad(cfgCmdStanzaCreate, argList);

        cmdStanzaCreate();

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when postmaster.pid exists");

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        harnessCfgLoad(cfgCmdBackup, argList);

        storagePutP(storageNewWriteP(storagePgWrite(), PG_FILE_POSTMASTERPID_STR), BUFSTRDEF("PID"));

        TEST_ERROR(
            cmdBackup(), PostmasterRunningError,
            "--no-online passed but postmaster.pid exists - looks like the postmaster is running. Shutdown the postmaster and try"
                " again, or use --force.");

        TEST_RESULT_LOG("P00   WARN: no prior backup exists, incr backup has been changed to full");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("offline full backup");

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
        strLstAddZ(argList, "--" CFGOPT_FORCE);
        harnessCfgLoad(cfgCmdBackup, argList);

        storagePutP(storageNewWriteP(storagePgWrite(), STRDEF("postgresql.conf")), BUFSTRDEF("CONFIGSTUFF"));

        TEST_RESULT_VOID(cmdBackup(), "backup");

        TEST_RESULT_LOG_FMT(
            "P00   WARN: no prior backup exists, incr backup has been changed to full\n"
            "P00   WARN: --no-online passed and postmaster.pid exists but --force was passed so backup will continue though it"
                " looks like the postmaster is running and the backup will probably not be consistent\n"
            "P01   INFO: backup file {[path]}/pg1/global/pg_control (8KB, 99%%) checksum %s\n"
            "P01   INFO: backup file {[path]}/pg1/postgresql.conf (11B, 100%%) checksum e3db315c260e79211b7b52587123b7aa060f30ab\n"
            "P00   INFO: full backup size = 8KB\n"
            "P00   INFO: new backup label = [FULL-1]",
            TEST_64BIT() ? "21e2ddc99cdf4cfca272eee4f38891146092e358" : "8bb70506d988a8698d9e8cf90736ada23634571b");

        // Remove postmaster.pid
        storageRemoveP(storagePgWrite(), PG_FILE_POSTMASTERPID_STR, .errorOnMissing = true);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when no files have changed");

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--" CFGOPT_COMPRESS);
        strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
        strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_DIFF);
        harnessCfgLoad(cfgCmdBackup, argList);

        TEST_ERROR(cmdBackup(), FileMissingError, "no files have changed since the last backup - this seems unlikely");

        TEST_RESULT_LOG(
            "P00   INFO: last backup label = [FULL-1], version = " PROJECT_VERSION "\n"
            "P00   WARN: diff backup cannot alter compress-type option to 'gz', reset to value in [FULL-1]\n"
            "P00   WARN: diff backup cannot alter hardlink option to 'true', reset to value in [FULL-1]");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("offline incr backup to test unresumable backup");

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
        strLstAddZ(argList, "--" CFGOPT_CHECKSUM_PAGE);
        strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_INCR);
        harnessCfgLoad(cfgCmdBackup, argList);

        storagePutP(storageNewWriteP(storagePgWrite(), PG_FILE_PGVERSION_STR), BUFSTRDEF("VER"));

        TEST_RESULT_VOID(cmdBackup(), "backup");

        TEST_RESULT_LOG(
            "P00   INFO: last backup label = [FULL-1], version = " PROJECT_VERSION "\n"
            "P00   WARN: incr backup cannot alter 'checksum-page' option to 'true', reset to 'false' from [FULL-1]\n"
            "P00   WARN: backup '[DIFF-1]' cannot be resumed: new backup type 'incr' does not match resumable backup type 'diff'\n"
            "P01   INFO: backup file {[path]}/pg1/PG_VERSION (3B, 100%) checksum c8663c2525f44b6d9c687fbceb4aafc63ed8b451\n"
            "P00 DETAIL: reference pg_data/global/pg_control to [FULL-1]\n"
            "P00 DETAIL: reference pg_data/postgresql.conf to [FULL-1]\n"
            "P00   INFO: incr backup size = 3B\n"
            "P00   INFO: new backup label = [INCR-1]");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("offline diff backup to test prior backup must be full");

        argList = strLstNew();
        strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
        strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
        strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
        strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
        strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
        strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
        strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_DIFF);
        harnessCfgLoad(cfgCmdBackup, argList);

        sleepMSec(MSEC_PER_SEC - (timeMSec() % MSEC_PER_SEC));
        storagePutP(storageNewWriteP(storagePgWrite(), PG_FILE_PGVERSION_STR), BUFSTRDEF("VR2"));

        TEST_RESULT_VOID(cmdBackup(), "backup");

        TEST_RESULT_LOG(
            "P00   INFO: last backup label = [FULL-1], version = " PROJECT_VERSION "\n"
            "P01   INFO: backup file {[path]}/pg1/PG_VERSION (3B, 100%) checksum 6f1894088c578e4f0b9888e8e8a997d93cbbc0c5\n"
            "P00 DETAIL: reference pg_data/global/pg_control to [FULL-1]\n"
            "P00 DETAIL: reference pg_data/postgresql.conf to [FULL-1]\n"
            "P00   INFO: diff backup size = 3B\n"
            "P00   INFO: new backup label = [DIFF-2]");
    }

    // *****************************************************************************************************************************
    if (testBegin("cmdBackup() online"))
    {
        const String *pg1Path = strNewFmt("%s/pg1", testPath());
        const String *repoPath = strNewFmt("%s/repo", testPath());
        const String *pg2Path = strNewFmt("%s/pg2", testPath());

        // Set log level to detail
        harnessLogLevelSet(logLevelDetail);

        // Replace percent complete and backup size since they can cause a lot of churn when files are added/removed
        hrnLogReplaceAdd(", [0-9]{1,3}%\\)", "[0-9]+%", "PCT", false);
        hrnLogReplaceAdd(" backup size = [0-9]+[A-Z]+", "[^ ]+$", "SIZE", false);

        // Replace checksums since they can differ between architectures (e.g. 32/64 bit)
        hrnLogReplaceAdd("\\) checksum [a-f0-9]{40}", "[a-f0-9]{40}$", "SHA1", false);

        // Backup start time epoch.  The idea is to not have backup times (and therefore labels) ever change.  Each backup added
        // should be separated by 100,000 seconds (1,000,000 after stanza-upgrade) but after the initial assignments this will only
        // be possible at the beginning and the end, so new backups added in the middle will average the start times of the prior
        // and next backup to get their start time.  Backups added to the beginning of the test will need to subtract from the
        // epoch.
        #define BACKUP_EPOCH                                        1570000000

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online 9.5 resume uncompressed full backup");

        time_t backupTimeStart = BACKUP_EPOCH;

        {
            // Create pg_control
            storagePutP(
                storageNewWriteP(
                    storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path)),
                    .timeModified = backupTimeStart),
                pgControlTestToBuffer((PgControl){.version = PG_VERSION_95, .systemId = 1000000000000000950}));

            // Create stanza
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
            harnessCfgLoad(cfgCmdStanzaCreate, argList);

            cmdStanzaCreate();

            // Load options
            argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_FULL);
            strLstAddZ(argList, "--" CFGOPT_STOP_AUTO);
            strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
            strLstAddZ(argList, "--no-" CFGOPT_ARCHIVE_CHECK);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Add files
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF("postgresql.conf"), .timeModified = backupTimeStart),
                BUFSTRDEF("CONFIGSTUFF"));
            storagePutP(
                storageNewWriteP(storagePgWrite(), PG_FILE_PGVERSION_STR, .timeModified = backupTimeStart),
                BUFSTRDEF(PG_VERSION_95_STR));
            storagePathCreateP(storagePgWrite(), pgWalPath(PG_VERSION_95), .noParentCreate = true);

            // Create a backup manifest that looks like a halted backup manifest
            Manifest *manifestResume = manifestNewBuild(storagePg(), PG_VERSION_95, true, false, NULL, NULL);
            ManifestData *manifestResumeData = (ManifestData *)manifestData(manifestResume);

            manifestResumeData->backupType = backupTypeFull;
            const String *resumeLabel = backupLabelCreate(backupTypeFull, NULL, backupTimeStart);
            manifestBackupLabelSet(manifestResume, resumeLabel);

            // Copy a file to be resumed that has not changed in the repo
            storageCopy(
                storageNewReadP(storagePg(), PG_FILE_PGVERSION_STR),
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/PG_VERSION", strPtr(resumeLabel))));

            strcpy(
                ((ManifestFile *)manifestFileFind(manifestResume, STRDEF("pg_data/PG_VERSION")))->checksumSha1,
                "06d06bb31b570b94d7b4325f511f853dbe771c21");

            // Save the resume manifest
            manifestSave(
                manifestResume,
                storageWriteIo(
                    storageNewWriteP(
                        storageRepoWrite(),
                        strNewFmt(STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE INFO_COPY_EXT, strPtr(resumeLabel)))));

            // Run backup
            testBackupPqScriptP(PG_VERSION_95, backupTimeStart);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            TEST_RESULT_LOG(
                "P00   INFO: execute exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000000105D944C000000000, lsn = 5d944c0/0\n"
                "P00   WARN: resumable backup 20191002-070640F of same type exists -- remove invalid files and resume\n"
                "P01   INFO: backup file {[path]}/pg1/global/pg_control (8KB, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/postgresql.conf (11B, [PCT]) checksum [SHA1]\n"
                "P01 DETAIL: checksum resumed file {[path]}/pg1/PG_VERSION (3B, [PCT]) checksum [SHA1]\n"
                "P00   INFO: full backup size = [SIZE]\n"
                "P00   INFO: execute exclusive pg_stop_backup() and wait for all WAL segments to archive\n"
                "P00   INFO: backup stop archive = 0000000105D944C000000000, lsn = 5d944c0/800000\n"
                "P00   INFO: new backup label = 20191002-070640F");

            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/latest")),
                ". {link, d=20191002-070640F}\n"
                "pg_data {path}\n"
                "pg_data/PG_VERSION {file, s=3}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control {file, s=8192}\n"
                "pg_data/pg_xlog {path}\n"
                "pg_data/postgresql.conf {file, s=11}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"06d06bb31b570b94d7b4325f511f853dbe771c21\",\"size\":3"
                    ",\"timestamp\":1570000000}\n"
                "pg_data/global/pg_control={\"size\":8192,\"timestamp\":1570000000}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\",\"size\":11"
                    ",\"timestamp\":1570000000}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_xlog={}\n",
                "compare file list");
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online resumed compressed 9.5 full backup");

        // Backup start time
        backupTimeStart = BACKUP_EPOCH + 100000;

        {
            // Load options
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_FULL);
            strLstAddZ(argList, "--" CFGOPT_STOP_AUTO);
            strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
            strLstAddZ(argList, "--" CFGOPT_ARCHIVE_COPY);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Create a backup manifest that looks like a halted backup manifest
            Manifest *manifestResume = manifestNewBuild(storagePg(), PG_VERSION_95, true, false, NULL, NULL);
            ManifestData *manifestResumeData = (ManifestData *)manifestData(manifestResume);

            manifestResumeData->backupType = backupTypeFull;
            manifestResumeData->backupOptionCompressType = compressTypeGz;
            const String *resumeLabel = backupLabelCreate(backupTypeFull, NULL, backupTimeStart);
            manifestBackupLabelSet(manifestResume, resumeLabel);

            // File exists in cluster and repo but not in the resume manifest
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF("not-in-resume"), .timeModified = backupTimeStart), BUFSTRDEF("TEST"));
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/not-in-resume.gz", strPtr(resumeLabel))),
                NULL);

            // Remove checksum from file so it won't be resumed
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/global/pg_control.gz", strPtr(resumeLabel))),
                NULL);

            ((ManifestFile *)manifestFileFind(manifestResume, STRDEF("pg_data/global/pg_control")))->checksumSha1[0] = 0;

            // Size does not match between cluster and resume manifest
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF("size-mismatch"), .timeModified = backupTimeStart), BUFSTRDEF("TEST"));
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/size-mismatch.gz", strPtr(resumeLabel))),
                NULL);
            manifestFileAdd(
                manifestResume, &(ManifestFile){
                    .name = STRDEF("pg_data/size-mismatch"), .checksumSha1 = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", .size = 33});

            // Time does not match between cluster and resume manifest
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF("time-mismatch"), .timeModified = backupTimeStart), BUFSTRDEF("TEST"));
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/time-mismatch.gz", strPtr(resumeLabel))),
                NULL);
            manifestFileAdd(
                manifestResume, &(ManifestFile){
                    .name = STRDEF("pg_data/time-mismatch"), .checksumSha1 = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", .size = 4,
                    .timestamp = backupTimeStart - 1});

            // Size is zero in cluster and resume manifest. ??? We'd like to remove this requirement after the migration.
            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF("zero-size"), .timeModified = backupTimeStart), NULL);
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/zero-size.gz", strPtr(resumeLabel))),
                BUFSTRDEF("ZERO-SIZE"));
            manifestFileAdd(
                manifestResume, &(ManifestFile){.name = STRDEF("pg_data/zero-size"), .size = 0, .timestamp = backupTimeStart});

            // Path is not in manifest
            storagePathCreateP(storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/bogus_path", strPtr(resumeLabel)));

            // File is not in manifest
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/global/bogus.gz", strPtr(resumeLabel))),
                NULL);

            // File has incorrect compression type
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/global/bogus", strPtr(resumeLabel))),
                NULL);

            // Save the resume manifest
            manifestSave(
                manifestResume,
                storageWriteIo(
                    storageNewWriteP(
                        storageRepoWrite(),
                        strNewFmt(STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE INFO_COPY_EXT, strPtr(resumeLabel)))));

            // Disable storageFeaturePath so paths will not be created before files are copied
            ((Storage *)storageRepoWrite())->interface.feature ^= 1 << storageFeaturePath;

            // Disable storageFeaturePathSync so paths will not be synced
            ((Storage *)storageRepoWrite())->interface.feature ^= 1 << storageFeaturePathSync;

            // Run backup
            testBackupPqScriptP(PG_VERSION_95, backupTimeStart);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            // Enable storage features
            ((Storage *)storageRepoWrite())->interface.feature |= 1 << storageFeaturePath;
            ((Storage *)storageRepoWrite())->interface.feature |= 1 << storageFeaturePathSync;

            TEST_RESULT_LOG(
                "P00   INFO: execute exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000000105D95D3000000000, lsn = 5d95d30/0\n"
                "P00   WARN: resumable backup 20191003-105320F of same type exists -- remove invalid files and resume\n"
                "P00 DETAIL: remove path '{[path]}/repo/backup/test1/20191003-105320F/pg_data/bogus_path' from resumed backup\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/global/bogus' from resumed backup"
                    " (mismatched compression type)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/global/bogus.gz' from resumed backup"
                    " (missing in manifest)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/global/pg_control.gz' from resumed"
                    " backup (no checksum in resumed manifest)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/not-in-resume.gz' from resumed backup"
                    " (missing in resumed manifest)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/size-mismatch.gz' from resumed backup"
                    " (mismatched size)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/time-mismatch.gz' from resumed backup"
                    " (mismatched timestamp)\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F/pg_data/zero-size.gz' from resumed backup"
                    " (zero size)\n"
                "P01   INFO: backup file {[path]}/pg1/global/pg_control (8KB, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/postgresql.conf (11B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/time-mismatch (4B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/size-mismatch (4B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/not-in-resume (4B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/PG_VERSION (3B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/zero-size (0B, [PCT])\n"
                "P00   INFO: full backup size = [SIZE]\n"
                "P00   INFO: execute exclusive pg_stop_backup() and wait for all WAL segments to archive\n"
                "P00   INFO: backup stop archive = 0000000105D95D3000000000, lsn = 5d95d30/800000\n"
                "P00   INFO: check archive for segment(s) 0000000105D95D3000000000:0000000105D95D3000000000\n"
                "P00   INFO: new backup label = 20191003-105320F");

            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/latest")),
                ". {link, d=20191003-105320F}\n"
                "pg_data {path}\n"
                "pg_data/PG_VERSION.gz {file, s=3}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control.gz {file, s=8192}\n"
                "pg_data/not-in-resume.gz {file, s=4}\n"
                "pg_data/pg_xlog {path}\n"
                "pg_data/pg_xlog/0000000105D95D3000000000.gz {file, s=16777216}\n"
                "pg_data/postgresql.conf.gz {file, s=11}\n"
                "pg_data/size-mismatch.gz {file, s=4}\n"
                "pg_data/time-mismatch.gz {file, s=4}\n"
                "pg_data/zero-size.gz {file, s=0}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"06d06bb31b570b94d7b4325f511f853dbe771c21\",\"size\":3"
                    ",\"timestamp\":1570000000}\n"
                "pg_data/global/pg_control={\"size\":8192,\"timestamp\":1570000000}\n"
                "pg_data/not-in-resume={\"checksum\":\"984816fd329622876e14907634264e6f332e9fb3\",\"size\":4"
                    ",\"timestamp\":1570100000}\n"
                "pg_data/pg_xlog/0000000105D95D3000000000={\"size\":16777216,\"timestamp\":1570100002}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\",\"size\":11"
                    ",\"timestamp\":1570000000}\n"
                "pg_data/size-mismatch={\"checksum\":\"984816fd329622876e14907634264e6f332e9fb3\",\"size\":4"
                    ",\"timestamp\":1570100000}\n"
                "pg_data/time-mismatch={\"checksum\":\"984816fd329622876e14907634264e6f332e9fb3\",\"size\":4"
                    ",\"timestamp\":1570100000}\n"
                "pg_data/zero-size={\"size\":0,\"timestamp\":1570100000}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_xlog={}\n",
                "compare file list");

            // Remove test files
            storageRemoveP(storagePgWrite(), STRDEF("not-in-resume"), .errorOnMissing = true);
            storageRemoveP(storagePgWrite(), STRDEF("size-mismatch"), .errorOnMissing = true);
            storageRemoveP(storagePgWrite(), STRDEF("time-mismatch"), .errorOnMissing = true);
            storageRemoveP(storagePgWrite(), STRDEF("zero-size"), .errorOnMissing = true);
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online resumed compressed 9.5 diff backup");

        backupTimeStart = BACKUP_EPOCH + 200000;

        {
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_DIFF);
            strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
            strLstAddZ(argList, "--" CFGOPT_STOP_AUTO);
            strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Load the previous manifest and null out the checksum-page option to be sure it gets set to false in this backup
            const String *manifestPriorFile = STRDEF(STORAGE_REPO_BACKUP "/latest/" BACKUP_MANIFEST_FILE);
            Manifest *manifestPrior = manifestNewLoad(storageReadIo(storageNewReadP(storageRepo(), manifestPriorFile)));
            ((ManifestData *)manifestData(manifestPrior))->backupOptionChecksumPage = NULL;
            manifestSave(manifestPrior, storageWriteIo(storageNewWriteP(storageRepoWrite(), manifestPriorFile)));

            // Create a backup manifest that looks like a halted backup manifest
            Manifest *manifestResume = manifestNewBuild(storagePg(), PG_VERSION_95, true, false, NULL, NULL);
            ManifestData *manifestResumeData = (ManifestData *)manifestData(manifestResume);

            manifestResumeData->backupType = backupTypeDiff;
            manifestResumeData->backupLabelPrior = manifestData(manifestPrior)->backupLabel;
            manifestResumeData->backupOptionCompressType = compressTypeGz;
            const String *resumeLabel = backupLabelCreate(backupTypeDiff, manifestData(manifestPrior)->backupLabel, backupTimeStart);
            manifestBackupLabelSet(manifestResume, resumeLabel);

            // Reference in manifest
            storagePutP(
                storageNewWriteP(storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/PG_VERSION.gz", strPtr(resumeLabel))),
                NULL);

            // Reference in resumed manifest
            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF("resume-ref"), .timeModified = backupTimeStart), NULL);
            storagePutP(
                storageNewWriteP(storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/resume-ref.gz", strPtr(resumeLabel))),
                NULL);
            manifestFileAdd(
                manifestResume, &(ManifestFile){.name = STRDEF("pg_data/resume-ref"), .size = 0, .reference = STRDEF("BOGUS")});

            // Time does not match between cluster and resume manifest (but resume because time is in future so delta enabled).  Note
            // also that the repo file is intenionally corrupt to generate a warning about corruption in the repository.
            storagePutP(
                storageNewWriteP(storagePgWrite(), STRDEF("time-mismatch2"), .timeModified = backupTimeStart + 100), BUFSTRDEF("TEST"));
            storagePutP(
                storageNewWriteP(
                    storageRepoWrite(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/time-mismatch2.gz", strPtr(resumeLabel))),
                NULL);
            manifestFileAdd(
                manifestResume, &(ManifestFile){
                    .name = STRDEF("pg_data/time-mismatch2"), .checksumSha1 = "984816fd329622876e14907634264e6f332e9fb3", .size = 4,
                    .timestamp = backupTimeStart});

            // Links are always removed on resume
            THROW_ON_SYS_ERROR(
                symlink(
                    "..",
                    strPtr(storagePathP(storageRepo(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/link", strPtr(resumeLabel))))) == -1,
                FileOpenError, "unable to create symlink");

            // Special files should not be in the repo
            TEST_SYSTEM_FMT(
                "mkfifo -m 666 %s",
                strPtr(storagePathP(storageRepo(), strNewFmt(STORAGE_REPO_BACKUP "/%s/pg_data/pipe", strPtr(resumeLabel)))));

            // Save the resume manifest
            manifestSave(
                manifestResume,
                storageWriteIo(
                    storageNewWriteP(
                        storageRepoWrite(),
                        strNewFmt(STORAGE_REPO_BACKUP "/%s/" BACKUP_MANIFEST_FILE INFO_COPY_EXT, strPtr(resumeLabel)))));

            // Run backup
            testBackupPqScriptP(PG_VERSION_95, backupTimeStart);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            // Check log
            TEST_RESULT_LOG(
                "P00   INFO: last backup label = 20191003-105320F, version = " PROJECT_VERSION "\n"
                "P00   WARN: diff backup cannot alter compress-type option to 'none', reset to value in 20191003-105320F\n"
                "P00   INFO: execute exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000000105D9759000000000, lsn = 5d97590/0\n"
                "P00   WARN: file 'time-mismatch2' has timestamp in the future, enabling delta checksum\n"
                "P00   WARN: resumable backup 20191003-105320F_20191004-144000D of same type exists"
                    " -- remove invalid files and resume\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F_20191004-144000D/pg_data/PG_VERSION.gz'"
                    " from resumed backup (reference in manifest)\n"
                "P00   WARN: remove special file '{[path]}/repo/backup/test1/20191003-105320F_20191004-144000D/pg_data/pipe'"
                    " from resumed backup\n"
                "P00 DETAIL: remove file '{[path]}/repo/backup/test1/20191003-105320F_20191004-144000D/pg_data/resume-ref.gz'"
                    " from resumed backup (reference in resumed manifest)\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/global/pg_control (8KB, [PCT]) checksum [SHA1]\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/postgresql.conf (11B, [PCT]) checksum [SHA1]\n"
                "P00   WARN: resumed backup file pg_data/time-mismatch2 does not have expected checksum"
                    " 984816fd329622876e14907634264e6f332e9fb3. The file will be recopied and backup will continue but this may be an"
                    " issue unless the resumed backup path in the repository is known to be corrupted.\n"
                "            NOTE: this does not indicate a problem with the PostgreSQL page checksums.\n"
                "P01   INFO: backup file {[path]}/pg1/time-mismatch2 (4B, [PCT]) checksum [SHA1]\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/PG_VERSION (3B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/resume-ref (0B, [PCT])\n"
                "P00 DETAIL: hardlink pg_data/PG_VERSION to 20191003-105320F\n"
                "P00 DETAIL: hardlink pg_data/global/pg_control to 20191003-105320F\n"
                "P00 DETAIL: hardlink pg_data/postgresql.conf to 20191003-105320F\n"
                "P00   INFO: diff backup size = [SIZE]\n"
                "P00   INFO: execute exclusive pg_stop_backup() and wait for all WAL segments to archive\n"
                "P00   INFO: backup stop archive = 0000000105D9759000000000, lsn = 5d97590/800000\n"
                    "P00   INFO: check archive for segment(s) 0000000105D9759000000000:0000000105D9759000000000\n"
                "P00   INFO: new backup label = 20191003-105320F_20191004-144000D");

            // Check repo directory
            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/latest")),
                ". {link, d=20191003-105320F_20191004-144000D}\n"
                "pg_data {path}\n"
                "pg_data/PG_VERSION.gz {file, s=3}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control.gz {file, s=8192}\n"
                "pg_data/pg_xlog {path}\n"
                "pg_data/postgresql.conf.gz {file, s=11}\n"
                "pg_data/resume-ref.gz {file, s=0}\n"
                "pg_data/time-mismatch2.gz {file, s=4}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"06d06bb31b570b94d7b4325f511f853dbe771c21\",\"reference\":\"20191003-105320F\""
                    ",\"size\":3,\"timestamp\":1570000000}\n"
                "pg_data/global/pg_control={\"reference\":\"20191003-105320F\",\"size\":8192,\"timestamp\":1570000000}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\""
                    ",\"reference\":\"20191003-105320F\",\"size\":11,\"timestamp\":1570000000}\n"
                "pg_data/resume-ref={\"size\":0,\"timestamp\":1570200000}\n"
                "pg_data/time-mismatch2={\"checksum\":\"984816fd329622876e14907634264e6f332e9fb3\",\"size\":4"
                    ",\"timestamp\":1570200100}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_xlog={}\n",
                "compare file list");

            // Remove test files
            storageRemoveP(storagePgWrite(), STRDEF("resume-ref"), .errorOnMissing = true);
            storageRemoveP(storagePgWrite(), STRDEF("time-mismatch2"), .errorOnMissing = true);
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online 9.6 backup-standby full backup");

        backupTimeStart = BACKUP_EPOCH + 1200000;

        {
            // Update pg_control
            storagePutP(
                storageNewWriteP(
                    storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path)),
                    .timeModified = backupTimeStart),
                pgControlTestToBuffer((PgControl){.version = PG_VERSION_96, .systemId = 1000000000000000960}));

            // Update version
            storagePutP(
                storageNewWriteP(storagePgWrite(), PG_FILE_PGVERSION_STR, .timeModified = backupTimeStart),
                BUFSTRDEF(PG_VERSION_96_STR));

            // Upgrade stanza
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
            harnessCfgLoad(cfgCmdStanzaUpgrade, argList);

            cmdStanzaUpgrade();

            // Load options
            argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG2_PATH "=%s", strPtr(pg2Path)));
            strLstAddZ(argList, "--" CFGOPT_PG2_PORT "=5433");
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--no-" CFGOPT_COMPRESS);
            strLstAddZ(argList, "--" CFGOPT_BACKUP_STANDBY);
            strLstAddZ(argList, "--" CFGOPT_START_FAST);
            strLstAddZ(argList, "--" CFGOPT_ARCHIVE_COPY);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Create file to copy from the standby. This file will be zero-length on the primary and non-zero-length on the standby
            // but no bytes will be copied.
            storagePutP(storageNewWriteP(storagePgIdWrite(1), STRDEF(PG_PATH_BASE "/1/1"), .timeModified = backupTimeStart), NULL);
            storagePutP(storageNewWriteP(storagePgIdWrite(2), STRDEF(PG_PATH_BASE "/1/1")), BUFSTRDEF("1234"));

            // Create file to copy from the standby. This file will be smaller on the primary than the standby and have no common
            // data in the bytes that exist on primary and standby.  If the file is copied from the primary instead of the standby
            // the checksum will change but not the size.
            storagePutP(
                storageNewWriteP(storagePgIdWrite(1), STRDEF(PG_PATH_BASE "/1/2"), .timeModified = backupTimeStart),
                BUFSTRDEF("DA"));
            storagePutP(storageNewWriteP(storagePgIdWrite(2), STRDEF(PG_PATH_BASE "/1/2")), BUFSTRDEF("5678"));

            // Create file to copy from the standby. This file will be larger on the primary than the standby and have no common
            // data in the bytes that exist on primary and standby.  If the file is copied from the primary instead of the standby
            // the checksum and size will change.
            storagePutP(
                storageNewWriteP(storagePgIdWrite(1), STRDEF(PG_PATH_BASE "/1/3"), .timeModified = backupTimeStart),
                BUFSTRDEF("TEST"));
            storagePutP(storageNewWriteP(storagePgIdWrite(2), STRDEF(PG_PATH_BASE "/1/3")), BUFSTRDEF("ABC"));

            // Create a file on the primary that does not exist on the standby to test that the file is removed from the manifest
            storagePutP(
                storageNewWriteP(storagePgIdWrite(1), STRDEF(PG_PATH_BASE "/1/0"), .timeModified = backupTimeStart),
                BUFSTRDEF("DATA"));

            // Set log level to warn because the following test uses multiple processes so the log order will not be deterministic
            harnessLogLevelSet(logLevelWarn);

            // Run backup but error on archive check
            testBackupPqScriptP(PG_VERSION_96, backupTimeStart, .noWal = true, .backupStandby = true);
            TEST_ERROR(
                cmdBackup(), ArchiveTimeoutError,
                "WAL segment 0000000105DA69C000000000 was not archived before the 100ms timeout\n"
                "HINT: check the archive_command to ensure that all options are correct (especially --stanza).\n"
                "HINT: check the PostgreSQL server log for errors.");

            // Remove halted backup so there's no resume
            storagePathRemoveP(storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191016-042640F"), .recurse = true);

            // Run backup
            testBackupPqScriptP(PG_VERSION_96, backupTimeStart, .backupStandby = true, .walCompressType = compressTypeGz);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            // Set log level back to detail
            harnessLogLevelSet(logLevelDetail);

            TEST_RESULT_LOG(
                "P00   WARN: no prior backup exists, incr backup has been changed to full");

            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/latest")),
                ". {link, d=20191016-042640F}\n"
                "pg_data {path}\n"
                "pg_data/PG_VERSION {file, s=3}\n"
                "pg_data/backup_label {file, s=17}\n"
                "pg_data/base {path}\n"
                "pg_data/base/1 {path}\n"
                "pg_data/base/1/1 {file, s=0}\n"
                "pg_data/base/1/2 {file, s=2}\n"
                "pg_data/base/1/3 {file, s=3}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control {file, s=8192}\n"
                "pg_data/pg_xlog {path}\n"
                "pg_data/pg_xlog/0000000105DA69C000000000 {file, s=16777216}\n"
                "pg_data/postgresql.conf {file, s=11}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"f5b7e6d36dc0113f61b36c700817d42b96f7b037\",\"size\":3"
                    ",\"timestamp\":1571200000}\n"
                "pg_data/backup_label={\"checksum\":\"8e6f41ac87a7514be96260d65bacbffb11be77dc\",\"size\":17"
                    ",\"timestamp\":1571200002}\n"
                "pg_data/base/1/1={\"master\":false,\"size\":0,\"timestamp\":1571200000}\n"
                "pg_data/base/1/2={\"checksum\":\"54ceb91256e8190e474aa752a6e0650a2df5ba37\",\"master\":false,\"size\":2"
                    ",\"timestamp\":1571200000}\n"
                "pg_data/base/1/3={\"checksum\":\"3c01bdbb26f358bab27f267924aa2c9a03fcfdb8\",\"master\":false,\"size\":3"
                    ",\"timestamp\":1571200000}\n"
                "pg_data/global/pg_control={\"size\":8192,\"timestamp\":1571200000}\n"
                "pg_data/pg_xlog/0000000105DA69C000000000={\"size\":16777216,\"timestamp\":1571200002}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\",\"size\":11"
                    ",\"timestamp\":1570000000}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/base={}\n"
                "pg_data/base/1={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_xlog={}\n",
                "compare file list");

            // Remove test files
            storagePathRemoveP(storagePgIdWrite(2), NULL, .recurse = true);
            storagePathRemoveP(storagePgWrite(), STRDEF("base/1"), .recurse = true);
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online 11 full backup with tablespaces and page checksums");

        backupTimeStart = BACKUP_EPOCH + 2200000;

        {
            // Update pg_control
            storagePutP(
                storageNewWriteP(
                    storageTest, strNewFmt("%s/" PG_PATH_GLOBAL "/" PG_FILE_PGCONTROL, strPtr(pg1Path)),
                    .timeModified = backupTimeStart),
                pgControlTestToBuffer(
                    (PgControl){
                        .version = PG_VERSION_11, .systemId = 1000000000000001100, .pageChecksum = true,
                        .walSegmentSize = 1024 * 1024}));

            // Update version
            storagePutP(
                storageNewWriteP(storagePgWrite(), PG_FILE_PGVERSION_STR, .timeModified = backupTimeStart),
                BUFSTRDEF(PG_VERSION_11_STR));

            // Update wal path
            storagePathRemoveP(storagePgWrite(), pgWalPath(PG_VERSION_95));
            storagePathCreateP(storagePgWrite(), pgWalPath(PG_VERSION_11), .noParentCreate = true);

            // Upgrade stanza
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--no-" CFGOPT_ONLINE);
            harnessCfgLoad(cfgCmdStanzaUpgrade, argList);

            cmdStanzaUpgrade();

            // Load options
            argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_FULL);
            strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
            strLstAddZ(argList, "--" CFGOPT_MANIFEST_SAVE_THRESHOLD "=1");
            strLstAddZ(argList, "--" CFGOPT_ARCHIVE_COPY);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Move pg1-path and put a link in its place. This tests that backup works when pg1-path is a symlink yet should be
            // completely invisible in the manifest and logging.
            TEST_SYSTEM_FMT("mv %s %s-data", strPtr(pg1Path), strPtr(pg1Path));
            TEST_SYSTEM_FMT("ln -s %s-data %s ", strPtr(pg1Path), strPtr(pg1Path));

            // Zeroed file which passes page checksums
            Buffer *relation = bufNew(PG_PAGE_SIZE_DEFAULT);
            memset(bufPtr(relation), 0, bufSize(relation));
            bufUsedSet(relation, bufSize(relation));

            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x00)) = (PageHeaderData){.pd_upper = 0};

            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF(PG_PATH_BASE "/1/1"), .timeModified = backupTimeStart), relation);

            // Zeroed file which will fail on alignment
            relation = bufNew(PG_PAGE_SIZE_DEFAULT + 1);
            memset(bufPtr(relation), 0, bufSize(relation));
            bufUsedSet(relation, bufSize(relation));

            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x00)) = (PageHeaderData){.pd_upper = 0};

            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF(PG_PATH_BASE "/1/2"), .timeModified = backupTimeStart), relation);

            // File with bad page checksums
            relation = bufNew(PG_PAGE_SIZE_DEFAULT * 4);
            memset(bufPtr(relation), 0, bufSize(relation));
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x00)) = (PageHeaderData){.pd_upper = 0xFF};
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x01)) = (PageHeaderData){.pd_upper = 0x00};
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x02)) = (PageHeaderData){.pd_upper = 0xFE};
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x03)) = (PageHeaderData){.pd_upper = 0xEF};
            bufUsedSet(relation, bufSize(relation));

            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF(PG_PATH_BASE "/1/3"), .timeModified = backupTimeStart), relation);

            // File with bad page checksum
            relation = bufNew(PG_PAGE_SIZE_DEFAULT * 3);
            memset(bufPtr(relation), 0, bufSize(relation));
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x00)) = (PageHeaderData){.pd_upper = 0x00};
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x01)) = (PageHeaderData){.pd_upper = 0x08};
            *(PageHeaderData *)(bufPtr(relation) + (PG_PAGE_SIZE_DEFAULT * 0x02)) = (PageHeaderData){.pd_upper = 0x00};
            bufUsedSet(relation, bufSize(relation));

            storagePutP(storageNewWriteP(storagePgWrite(), STRDEF(PG_PATH_BASE "/1/4"), .timeModified = backupTimeStart), relation);

            // Add a tablespace
            storagePathCreateP(storagePgWrite(), STRDEF(PG_PATH_PGTBLSPC));
            THROW_ON_SYS_ERROR(
                symlink("../../pg1-tblspc/32768", strPtr(storagePathP(storagePg(), STRDEF(PG_PATH_PGTBLSPC "/32768")))) == -1,
                FileOpenError, "unable to create symlink");

            storagePutP(
                storageNewWriteP(
                    storageTest, strNewFmt("pg1-tblspc/32768/%s/1/5", strPtr(pgTablespaceId(PG_VERSION_11))),
                    .timeModified = backupTimeStart),
                NULL);

            // Disable storageFeatureSymLink so tablespace (and latest) symlinks will not be created
            ((Storage *)storageRepoWrite())->interface.feature ^= 1 << storageFeatureSymLink;

            // Disable storageFeatureHardLink so hardlinks will not be created
            ((Storage *)storageRepoWrite())->interface.feature ^= 1 << storageFeatureHardLink;

            // Run backup
            testBackupPqScriptP(PG_VERSION_11, backupTimeStart, .walCompressType = compressTypeGz, .walTotal = 3);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            // Reset storage features
            ((Storage *)storageRepoWrite())->interface.feature |= 1 << storageFeatureSymLink;
            ((Storage *)storageRepoWrite())->interface.feature |= 1 << storageFeatureHardLink;

            TEST_RESULT_LOG(
                "P00   INFO: execute non-exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000000105DB5DE000000000, lsn = 5db5de0/0\n"
                "P01   INFO: backup file {[path]}/pg1/base/1/3 (32KB, [PCT]) checksum [SHA1]\n"
                "P00   WARN: invalid page checksums found in file {[path]}/pg1/base/1/3 at pages 0, 2-3\n"
                "P01   INFO: backup file {[path]}/pg1/base/1/4 (24KB, [PCT]) checksum [SHA1]\n"
                "P00   WARN: invalid page checksum found in file {[path]}/pg1/base/1/4 at page 1\n"
                "P01   INFO: backup file {[path]}/pg1/base/1/2 (8KB, [PCT]) checksum [SHA1]\n"
                "P00   WARN: page misalignment in file {[path]}/pg1/base/1/2: file size 8193 is not divisible by page size 8192\n"
                "P01   INFO: backup file {[path]}/pg1/global/pg_control (8KB, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/base/1/1 (8KB, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/postgresql.conf (11B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/PG_VERSION (2B, [PCT]) checksum [SHA1]\n"
                "P01   INFO: backup file {[path]}/pg1/pg_tblspc/32768/PG_11_201809051/1/5 (0B, [PCT])\n"
                "P00   INFO: full backup size = [SIZE]\n"
                "P00   INFO: execute non-exclusive pg_stop_backup() and wait for all WAL segments to archive\n"
                "P00   INFO: backup stop archive = 0000000105DB5DE000000002, lsn = 5db5de0/280000\n"
                "P00 DETAIL: wrote 'backup_label' file returned from pg_stop_backup()\n"
                "P00   INFO: check archive for segment(s) 0000000105DB5DE000000000:0000000105DB5DE000000002\n"
                "P00   INFO: new backup label = 20191027-181320F");

            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/20191027-181320F")),
                "pg_data {path}\n"
                "pg_data/PG_VERSION.gz {file, s=2}\n"
                "pg_data/backup_label.gz {file, s=17}\n"
                "pg_data/base {path}\n"
                "pg_data/base/1 {path}\n"
                "pg_data/base/1/1.gz {file, s=8192}\n"
                "pg_data/base/1/2.gz {file, s=8193}\n"
                "pg_data/base/1/3.gz {file, s=32768}\n"
                "pg_data/base/1/4.gz {file, s=24576}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control.gz {file, s=8192}\n"
                "pg_data/pg_tblspc {path}\n"
                "pg_data/pg_wal {path}\n"
                "pg_data/pg_wal/0000000105DB5DE000000000.gz {file, s=1048576}\n"
                "pg_data/pg_wal/0000000105DB5DE000000001.gz {file, s=1048576}\n"
                "pg_data/pg_wal/0000000105DB5DE000000002.gz {file, s=1048576}\n"
                "pg_data/postgresql.conf.gz {file, s=11}\n"
                "pg_tblspc {path}\n"
                "pg_tblspc/32768 {path}\n"
                "pg_tblspc/32768/PG_11_201809051 {path}\n"
                "pg_tblspc/32768/PG_11_201809051/1 {path}\n"
                "pg_tblspc/32768/PG_11_201809051/1/5.gz {file, s=0}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "pg_tblspc/32768={\"path\":\"../../pg1-tblspc/32768\",\"tablespace-id\":\"32768\",\"tablespace-name\":\"tblspc32768\",\"type\":\"link\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"17ba0791499db908433b80f37c5fbc89b870084b\",\"size\":2"
                    ",\"timestamp\":1572200000}\n"
                "pg_data/backup_label={\"checksum\":\"8e6f41ac87a7514be96260d65bacbffb11be77dc\",\"size\":17"
                    ",\"timestamp\":1572200002}\n"
                "pg_data/base/1/1={\"checksum\":\"0631457264ff7f8d5fb1edc2c0211992a67c73e6\",\"checksum-page\":true"
                    ",\"master\":false,\"size\":8192,\"timestamp\":1572200000}\n"
                "pg_data/base/1/2={\"checksum\":\"8beb58e08394fe665fb04a17b4003faa3802760b\",\"checksum-page\":false"
                    ",\"master\":false,\"size\":8193,\"timestamp\":1572200000}\n"
                "pg_data/base/1/3={\"checksum\":\"73e537a445ad34eab4b292ac6aa07b8ce14e8421\",\"checksum-page\":false"
                    ",\"checksum-page-error\":[0,[2,3]],\"master\":false,\"size\":32768,\"timestamp\":1572200000}\n"
                "pg_data/base/1/4={\"checksum\":\"ba233be7198b3115f0480fa5274448f2a2fc2af1\",\"checksum-page\":false"
                    ",\"checksum-page-error\":[1],\"master\":false,\"size\":24576,\"timestamp\":1572200000}\n"
                "pg_data/global/pg_control={\"size\":8192,\"timestamp\":1572200000}\n"
                "pg_data/pg_wal/0000000105DB5DE000000000={\"size\":1048576,\"timestamp\":1572200002}\n"
                "pg_data/pg_wal/0000000105DB5DE000000001={\"size\":1048576,\"timestamp\":1572200002}\n"
                "pg_data/pg_wal/0000000105DB5DE000000002={\"size\":1048576,\"timestamp\":1572200002}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\",\"size\":11"
                    ",\"timestamp\":1570000000}\n"
                "pg_tblspc/32768/PG_11_201809051/1/5={\"checksum-page\":true,\"master\":false,\"size\":0"
                    ",\"timestamp\":1572200000}\n"
                "\n"
                "[target:link]\n"
                "pg_data/pg_tblspc/32768={\"destination\":\"../../pg1-tblspc/32768\"}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/base={}\n"
                "pg_data/base/1={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_tblspc={}\n"
                "pg_data/pg_wal={}\n"
                "pg_tblspc={}\n"
                "pg_tblspc/32768={}\n"
                "pg_tblspc/32768/PG_11_201809051={}\n"
                "pg_tblspc/32768/PG_11_201809051/1={}\n",
                "compare file list");

            // Remove test files
            storagePathRemoveP(storagePgWrite(), STRDEF("base/1"), .recurse = true);
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("error when pg_control not present");

        backupTimeStart = BACKUP_EPOCH + 2300000;

        {
            // Load options
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_INCR);
            strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Run backup
            testBackupPqScriptP(PG_VERSION_11, backupTimeStart, .errorAfterStart = true);
            TEST_ERROR(
                cmdBackup(), FileMissingError,
                "pg_control must be present in all online backups\n"
                "HINT: is something wrong with the clock or filesystem timestamps?");

            // Check log
            TEST_RESULT_LOG(
                "P00   INFO: last backup label = 20191027-181320F, version = " PROJECT_VERSION "\n"
                "P00   INFO: execute non-exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000000105DB764000000000, lsn = 5db7640/0");

            // Remove partial backup so it won't be resumed (since it errored before any checksums were written)
            storagePathRemoveP(
                storageRepoWrite(), STRDEF(STORAGE_REPO_BACKUP "/20191027-181320F_20191028-220000I"), .recurse = true);
        }

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("online 11 incr backup with tablespaces");

        backupTimeStart = BACKUP_EPOCH + 2400000;

        {
            // Load options
            StringList *argList = strLstNew();
            strLstAddZ(argList, "--" CFGOPT_STANZA "=test1");
            strLstAdd(argList, strNewFmt("--" CFGOPT_REPO1_PATH "=%s", strPtr(repoPath)));
            strLstAdd(argList, strNewFmt("--" CFGOPT_PG1_PATH "=%s", strPtr(pg1Path)));
            strLstAddZ(argList, "--" CFGOPT_REPO1_RETENTION_FULL "=1");
            strLstAddZ(argList, "--" CFGOPT_TYPE "=" BACKUP_TYPE_INCR);
            strLstAddZ(argList, "--" CFGOPT_DELTA);
            strLstAddZ(argList, "--" CFGOPT_REPO1_HARDLINK);
            harnessCfgLoad(cfgCmdBackup, argList);

            // Update pg_control timestamp
            THROW_ON_SYS_ERROR(
                utime(
                    strPtr(storagePathP(storagePg(), STRDEF("global/pg_control"))),
                    &(struct utimbuf){.actime = backupTimeStart, .modtime = backupTimeStart}) != 0, FileWriteError,
                "unable to set time");

            // Run backup.  Make sure that the timeline selected converts to hexdecimal that can't be interpreted as decimal.
            testBackupPqScriptP(PG_VERSION_11, backupTimeStart, .timeline = 0x2C);
            TEST_RESULT_VOID(cmdBackup(), "backup");

            TEST_RESULT_LOG(
                "P00   INFO: last backup label = 20191027-181320F, version = " PROJECT_VERSION "\n"
                "P00   INFO: execute non-exclusive pg_start_backup(): backup begins after the next regular checkpoint completes\n"
                "P00   INFO: backup start archive = 0000002C05DB8EB000000000, lsn = 5db8eb0/0\n"
                "P00   WARN: a timeline switch has occurred since the 20191027-181320F backup, enabling delta checksum\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/global/pg_control (8KB, [PCT]) checksum [SHA1]\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/postgresql.conf (11B, [PCT]) checksum [SHA1]\n"
                "P01 DETAIL: match file from prior backup {[path]}/pg1/PG_VERSION (2B, [PCT]) checksum [SHA1]\n"
                "P00 DETAIL: hardlink pg_data/PG_VERSION to 20191027-181320F\n"
                "P00 DETAIL: hardlink pg_data/global/pg_control to 20191027-181320F\n"
                "P00 DETAIL: hardlink pg_data/postgresql.conf to 20191027-181320F\n"
                "P00 DETAIL: hardlink pg_tblspc/32768/PG_11_201809051/1/5 to 20191027-181320F\n"
                "P00   INFO: incr backup size = [SIZE]\n"
                "P00   INFO: execute non-exclusive pg_stop_backup() and wait for all WAL segments to archive\n"
                "P00   INFO: backup stop archive = 0000002C05DB8EB000000000, lsn = 5db8eb0/80000\n"
                "P00 DETAIL: wrote 'backup_label' file returned from pg_stop_backup()\n"
                "P00   INFO: check archive for segment(s) 0000002C05DB8EB000000000:0000002C05DB8EB000000000\n"
                "P00   INFO: new backup label = 20191027-181320F_20191030-014640I");

            TEST_RESULT_STR_Z_KEYRPL(
                testBackupValidate(storageRepo(), STRDEF(STORAGE_REPO_BACKUP "/latest")),
                ". {link, d=20191027-181320F_20191030-014640I}\n"
                "pg_data {path}\n"
                "pg_data/PG_VERSION.gz {file, s=2}\n"
                "pg_data/backup_label.gz {file, s=17}\n"
                "pg_data/base {path}\n"
                "pg_data/global {path}\n"
                "pg_data/global/pg_control.gz {file, s=8192}\n"
                "pg_data/pg_tblspc {path}\n"
                "pg_data/pg_tblspc/32768 {link, d=../../pg_tblspc/32768}\n"
                "pg_data/pg_wal {path}\n"
                "pg_data/postgresql.conf.gz {file, s=11}\n"
                "pg_tblspc {path}\n"
                "pg_tblspc/32768 {path}\n"
                "pg_tblspc/32768/PG_11_201809051 {path}\n"
                "pg_tblspc/32768/PG_11_201809051/1 {path}\n"
                "pg_tblspc/32768/PG_11_201809051/1/5.gz {file, s=0}\n"
                "--------\n"
                "[backup:target]\n"
                "pg_data={\"path\":\"{[path]}/pg1\",\"type\":\"path\"}\n"
                "pg_tblspc/32768={\"path\":\"../../pg1-tblspc/32768\",\"tablespace-id\":\"32768\",\"tablespace-name\":\"tblspc32768\",\"type\":\"link\"}\n"
                "\n"
                "[target:file]\n"
                "pg_data/PG_VERSION={\"checksum\":\"17ba0791499db908433b80f37c5fbc89b870084b\",\"reference\":\"20191027-181320F\""
                    ",\"size\":2,\"timestamp\":1572200000}\n"
                "pg_data/backup_label={\"checksum\":\"8e6f41ac87a7514be96260d65bacbffb11be77dc\",\"size\":17"
                    ",\"timestamp\":1572400002}\n"
                "pg_data/global/pg_control={\"reference\":\"20191027-181320F\",\"size\":8192,\"timestamp\":1572400000}\n"
                "pg_data/postgresql.conf={\"checksum\":\"e3db315c260e79211b7b52587123b7aa060f30ab\""
                    ",\"reference\":\"20191027-181320F\",\"size\":11,\"timestamp\":1570000000}\n"
                "pg_tblspc/32768/PG_11_201809051/1/5={\"checksum-page\":true,\"master\":false,\"reference\":\"20191027-181320F\""
                    ",\"size\":0,\"timestamp\":1572200000}\n"
                "\n"
                "[target:link]\n"
                "pg_data/pg_tblspc/32768={\"destination\":\"../../pg1-tblspc/32768\"}\n"
                "\n"
                "[target:path]\n"
                "pg_data={}\n"
                "pg_data/base={}\n"
                "pg_data/global={}\n"
                "pg_data/pg_tblspc={}\n"
                "pg_data/pg_wal={}\n"
                "pg_tblspc={}\n"
                "pg_tblspc/32768={}\n"
                "pg_tblspc/32768/PG_11_201809051={}\n"
                "pg_tblspc/32768/PG_11_201809051/1={}\n",
                "compare file list");

            // Remove test files
            storagePathRemoveP(storagePgWrite(), STRDEF("base/1"), .recurse = true);
        }
    }

    FUNCTION_HARNESS_RESULT_VOID();
}
