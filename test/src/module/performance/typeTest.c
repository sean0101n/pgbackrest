/***********************************************************************************************************************************
Test Type Performance

Test the performance of various types and data structures.  Generally speaking, the starting values should be high enough to "blow
up" in terms of execution time if there are performance problems without taking very long if everything is running smoothly.

These starting values can then be scaled up for profiling and stress testing as needed.  In general we hope to scale to 1000 without
running out of memory on the test systems or taking an undue amount of time.  It should be noted that in this context scaling to
1000 is nowhere near to turning it up to 11.
***********************************************************************************************************************************/
#include "common/ini.h"
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "common/time.h"
#include "common/type/list.h"
#include "info/manifest.h"

#include "common/harnessInfo.h"

/***********************************************************************************************************************************
Test sort comparator
***********************************************************************************************************************************/
static int
testComparator(const void *item1, const void *item2)
{
    int int1 = *(int *)item1;
    int int2 = *(int *)item2;

    if (int1 < int2)
        return -1;

    if (int1 > int2)
        return 1;

    return 0;
}

/***********************************************************************************************************************************
Test callback to count ini load results
***********************************************************************************************************************************/
static void
testIniLoadCountCallback(void *data, const String *section, const String *key, const String *value)
{
    (*(unsigned int *)data)++;
    (void)section;
    (void)key;
    (void)value;
}

/***********************************************************************************************************************************
Test Run
***********************************************************************************************************************************/
void
testRun(void)
{
    FUNCTION_HARNESS_VOID();

    // *****************************************************************************************************************************
    if (testBegin("lstFind()"))
    {
        CHECK(testScale() <= 10000);
        int testMax = 100000 * (int)testScale();

        // Generate a large list of values (use int instead of string so there fewer allocations)
        List *list = lstNewP(sizeof(int), .comparator = testComparator);

        for (int listIdx = 0; listIdx < testMax; listIdx++)
            lstAdd(list, &listIdx);

        CHECK(lstSize(list) == (unsigned int)testMax);

        TEST_LOG_FMT("generated %d item list", testMax);

        // Search for all values with an ascending sort
        lstSort(list, sortOrderAsc);

        TimeMSec timeBegin = timeMSec();

        for (int listIdx = 0; listIdx < testMax; listIdx++)
            CHECK(*(int *)lstFind(list, &listIdx) == listIdx);

        TEST_LOG_FMT("asc search completed in %ums", (unsigned int)(timeMSec() - timeBegin));

        // Search for all values with an descending sort
        lstSort(list, sortOrderDesc);

        timeBegin = timeMSec();

        for (int listIdx = 0; listIdx < testMax; listIdx++)
            CHECK(*(int *)lstFind(list, &listIdx) == listIdx);

        TEST_LOG_FMT("desc search completed in %ums", (unsigned int)(timeMSec() - timeBegin));
    }

    // *****************************************************************************************************************************
    if (testBegin("iniLoad()"))
    {
        CHECK(testScale() <= 10000);

        String *iniStr = strNew("[section1]\n");
        unsigned int iniMax = 100000 * (unsigned int)testScale();

        for (unsigned int keyIdx = 0; keyIdx < iniMax; keyIdx++)
            strCatFmt(iniStr, "key%u=value%u\n", keyIdx, keyIdx);

        TEST_LOG_FMT("ini size = %s, keys = %u", strPtr(strSizeFormat(strSize(iniStr))), iniMax);

        TimeMSec timeBegin = timeMSec();
        unsigned int iniTotal = 0;

        TEST_RESULT_VOID(iniLoad(ioBufferReadNew(BUFSTR(iniStr)), testIniLoadCountCallback, &iniTotal), "parse ini");
        TEST_LOG_FMT("parse completed in %ums", (unsigned int)(timeMSec() - timeBegin));
        TEST_RESULT_INT(iniTotal, iniMax, "    check ini total");
    }

    // Load/save a larger manifest to test performance and memory usage.  The default sizing is for a "typical" cluster but this can
    // be scaled to test larger cluster sizes.
    // *****************************************************************************************************************************
    if (testBegin("manifestNewLoad()/manifestSave()"))
    {
        CHECK(testScale() <= 1000000);

        // Manifest with all features
        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("generate manifest");

        String *manifestStr = strNew
        (
            "[backup]\n"
            "backup-label=\"20190818-084502F_20190820-084502D\"\n"
            "backup-prior=\"20190818-084502F\"\n"
            "backup-timestamp-copy-start=1566290707\n"
            "backup-timestamp-start=1566290702\n"
            "backup-timestamp-stop=1566290710\n"
            "backup-type=\"diff\"\n"
            "\n"
            "[backup:db]\n"
            "db-catalog-version=201809051\n"
            "db-control-version=1100\n"
            "db-id=2\n"
            "db-system-id=6689162560678426440\n"
            "db-version=\"11\"\n"
            "\n"
            "[backup:option]\n"
            "option-archive-check=true\n"
            "option-archive-copy=false\n"
            "option-backup-standby=false\n"
            "option-buffer-size=1048576\n"
            "option-checksum-page=true\n"
            "option-compress=true\n"
            "option-compress-level=9\n"
            "option-compress-level-network=3\n"
            "option-delta=false\n"
            "option-hardlink=false\n"
            "option-online=false\n"
            "option-process-max=2\n"
            "\n"
            "[backup:target]\n"
            "pg_data={\"path\":\"/pg/base\",\"type\":\"path\"}\n");

        for (unsigned int linkIdx = 0; linkIdx < 1; linkIdx++)
            strCatFmt(manifestStr, "pg_data/pg_stat%u={\"path\":\"../pg_stat\",\"type\":\"link\"}\n", linkIdx);

        strCat(
            manifestStr,
            "\n"
            "[target:file]\n");

        unsigned int fileTotal = 100000 * (unsigned int)testScale();

        // Because of the way the filenames are formatted they will end up badly out of order.  We'll be depending on the sort after
        // load the fix this.  Normally the files won't need sorting, but a collation issue could well cause problems for us without
        // it.
        for (unsigned int fileIdx = 0; fileIdx < fileTotal; fileIdx++)
        {
            strCatFmt(
                manifestStr,
                "pg_data/base/16384/%u={\"checksum\":\"184473f470864e067ee3a22e64b47b0a1c356f29\",\"size\":16384"
                    ",\"timestamp\":1565282114}\n",
                16384 + fileIdx);
        }

        strCat(
            manifestStr,
            "\n"
            "[target:file:default]\n"
            "group=\"postgres\"\n"
            "master=false\n"
            "mode=\"0600\"\n"
            "user=\"postgres\"\n"
            "\n"
            "[target:link]\n"
            "pg_data/pg_stat={\"destination\":\"../pg_stat\"}\n"
            "\n"
            "[target:link:default]\n"
            "group=\"postgres\"\n"
            "user=\"postgres\"\n"
            "\n"
            "[target:path]\n"
            "pg_data={}\n"
            "pg_data/base={}\n"
            "pg_data/base/1={}\n"
            "pg_data/base/13124={}\n"
            "pg_data/base/13125={}\n"
            "pg_data/base/16391={}\n"
            "pg_data/global={}\n"
            "pg_data/pg_commit_ts={}\n"
            "pg_data/pg_dynshmem={}\n"
            "pg_data/pg_logical={}\n"
            "pg_data/pg_logical/mappings={}\n"
            "pg_data/pg_logical/snapshots={}\n"
            "pg_data/pg_multixact={}\n"
            "pg_data/pg_multixact/members={}\n"
            "pg_data/pg_multixact/offsets={}\n"
            "pg_data/pg_notify={}\n"
            "pg_data/pg_replslot={}\n"
            "pg_data/pg_serial={}\n"
            "pg_data/pg_snapshots={}\n"
            "pg_data/pg_stat={}\n"
            "pg_data/pg_stat_tmp={}\n"
            "pg_data/pg_subtrans={}\n"
            "pg_data/pg_tblspc={}\n"
            "pg_data/pg_twophase={}\n"
            "pg_data/pg_wal={}\n"
            "pg_data/pg_wal/archive_status={}\n"
            "pg_data/pg_xact={}\n"
            "\n"
            "[target:path:default]\n"
            "group=\"postgres\"\n"
            "mode=\"0700\"\n"
            "user=\"postgres\"\n"
        );

        const Buffer *contentLoad = harnessInfoChecksum(manifestStr);

        TEST_LOG_FMT("%s manifest generated with %u files", strPtr(strSizeFormat(bufUsed(contentLoad))), fileTotal);

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("load manifest");

        TimeMSec timeBegin = timeMSec();
        Manifest *manifest = manifestNewLoad(ioBufferReadNew(contentLoad));
        TEST_LOG_FMT("completed in %ums", (unsigned int)(timeMSec() - timeBegin));

        TEST_RESULT_UINT(manifestFileTotal(manifest), fileTotal, "   check file total");

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("save manifest");

        Buffer *contentSave = bufNew(0);
        timeBegin = timeMSec();
        manifestSave(manifest, ioBufferWriteNew(contentSave));
        TEST_LOG_FMT("completed in %ums", (unsigned int)(timeMSec() - timeBegin));

        // -------------------------------------------------------------------------------------------------------------------------
        TEST_TITLE("find all files");

        timeBegin = timeMSec();

        for (unsigned int fileIdx = 0; fileIdx < manifestFileTotal(manifest); fileIdx++)
        {
            const ManifestFile *file = manifestFile(manifest, fileIdx);
            CHECK(file == manifestFileFind(manifest, file->name));
        }

        TEST_LOG_FMT("completed in %ums", (unsigned int)(timeMSec() - timeBegin));
    }

    FUNCTION_HARNESS_RESULT_VOID();
}
