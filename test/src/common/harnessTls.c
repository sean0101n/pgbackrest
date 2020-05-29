/***********************************************************************************************************************************
TLS Test Harness
***********************************************************************************************************************************/
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/conf.h>
#include <openssl/ssl.h>

#include "common/crypto/common.h"
#include "common/error.h"
#include "common/io/socket/session.h"
#include "common/io/tls/session.intern.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/json.h"
#include "common/wait.h"

#include "common/harnessDebug.h"
#include "common/harnessTest.h"
#include "common/harnessTls.h"

/***********************************************************************************************************************************
Command enum
***********************************************************************************************************************************/
typedef enum
{
    hrnTlsCmdAbort,
    hrnTlsCmdAccept,
    hrnTlsCmdClose,
    hrnTlsCmdDone,
    hrnTlsCmdExpect,
    hrnTlsCmdReply,
    hrnTlsCmdSleep,
} HrnTlsCmd;

/***********************************************************************************************************************************
Constants
***********************************************************************************************************************************/
#define TLS_TEST_HOST                                               "tls.test.pgbackrest.org"
#define TLS_CERT_TEST_KEY                                           TLS_CERT_FAKE_PATH "/pgbackrest-test.key"

/***********************************************************************************************************************************
Local variables
***********************************************************************************************************************************/
static struct
{
    IoWrite *clientWrite;                                           // Write interface for client to send commands to the server
} hrnTlsLocal;

/***********************************************************************************************************************************
Send commands to the server
***********************************************************************************************************************************/
static void
hrnTlsServerCommand(HrnTlsCmd cmd, const Variant *data)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(ENUM, cmd);
        FUNCTION_HARNESS_PARAM(VARIANT, data);
    FUNCTION_HARNESS_END();

    ASSERT(hrnTlsLocal.clientWrite != NULL);

    ioWriteStrLine(hrnTlsLocal.clientWrite, jsonFromUInt(cmd));
    ioWriteStrLine(hrnTlsLocal.clientWrite, jsonFromVar(data));
    ioWriteFlush(hrnTlsLocal.clientWrite);

    FUNCTION_HARNESS_RESULT_VOID();
}

/**********************************************************************************************************************************/
void hrnTlsClientBegin(IoWrite *write)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(IO_WRITE, write);
    FUNCTION_HARNESS_END();

    ASSERT(hrnTlsLocal.clientWrite == NULL);
    ASSERT(write != NULL);

    hrnTlsLocal.clientWrite = write;
    ioWriteOpen(write);

    FUNCTION_HARNESS_RESULT_VOID();
}

void hrnTlsClientEnd(void)
{
    FUNCTION_HARNESS_VOID();

    ASSERT(hrnTlsLocal.clientWrite != NULL);

    hrnTlsServerCommand(hrnTlsCmdDone, NULL);
    hrnTlsLocal.clientWrite = NULL;

    FUNCTION_HARNESS_RESULT_VOID();
}

/**********************************************************************************************************************************/
void
hrnTlsServerAbort(void)
{
    FUNCTION_HARNESS_VOID();

    hrnTlsServerCommand(hrnTlsCmdAbort, NULL);

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerAccept(void)
{
    FUNCTION_HARNESS_VOID();

    hrnTlsServerCommand(hrnTlsCmdAccept, NULL);

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerClose()
{
    FUNCTION_HARNESS_VOID();

    hrnTlsServerCommand(hrnTlsCmdClose, NULL);

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerExpect(const String *data)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(STRING, data);
    FUNCTION_HARNESS_END();

    ASSERT(data != NULL);

    hrnTlsServerCommand(hrnTlsCmdExpect, VARSTR(data));

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerExpectZ(const char *data)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(STRINGZ, data);
    FUNCTION_HARNESS_END();

    ASSERT(data != NULL);

    hrnTlsServerCommand(hrnTlsCmdExpect, VARSTRZ(data));

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerReply(const String *data)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(STRING, data);
    FUNCTION_HARNESS_END();

    ASSERT(data != NULL);

    hrnTlsServerCommand(hrnTlsCmdReply, VARSTR(data));

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerReplyZ(const char *data)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(STRINGZ, data);
    FUNCTION_HARNESS_END();

    ASSERT(data != NULL);

    hrnTlsServerCommand(hrnTlsCmdReply, VARSTRZ(data));

    FUNCTION_HARNESS_RESULT_VOID();
}

void
hrnTlsServerSleep(TimeMSec sleepMs)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(TIME_MSEC, sleepMs);
    FUNCTION_HARNESS_END();

    ASSERT(sleepMs > 0);

    hrnTlsServerCommand(hrnTlsCmdSleep, VARUINT64(sleepMs));

    FUNCTION_HARNESS_RESULT_VOID();
}

/**********************************************************************************************************************************/
void hrnTlsServerRunParam(IoRead *read, const String *certificate, const String *key)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(IO_READ, read);
        FUNCTION_HARNESS_PARAM(STRING, certificate);
        FUNCTION_HARNESS_PARAM(STRING, key);
    FUNCTION_HARNESS_END();

    ASSERT(read != NULL);
    ASSERT(certificate != NULL);
    ASSERT(key != NULL);

    // Open read connection to client
    ioReadOpen(read);

    // Add test hosts
    if (testContainer())
    {
        if (system("echo \"127.0.0.1 " TLS_TEST_HOST "\" | sudo tee -a /etc/hosts > /dev/null") != 0)
            THROW(AssertError, "unable to add test host to /etc/hosts");
    }

    // Initialize ssl and create a context
    cryptoInit();

    const SSL_METHOD *method = SSLv23_method();
    cryptoError(method == NULL, "unable to load TLS method");

    SSL_CTX *serverContext = SSL_CTX_new(method);
    cryptoError(serverContext == NULL, "unable to create TLS context");

    // Configure the context by setting key and cert
    cryptoError(
        SSL_CTX_use_certificate_file(serverContext, strPtr(certificate), SSL_FILETYPE_PEM) <= 0,
        "unable to load server certificate");
    cryptoError(
        SSL_CTX_use_PrivateKey_file(serverContext, strPtr(key), SSL_FILETYPE_PEM) <= 0, "unable to load server private key");

    // Create the socket
    int serverSocket;

    struct sockaddr_in address;

    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)hrnTlsServerPort());
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((serverSocket = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        THROW_SYS_ERROR(AssertError, "unable to create socket");

    // Set the address as reusable so we can bind again in the same process for testing
    int reuseAddr = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddr, sizeof(reuseAddr));

    // Bind the address.  It might take a bit to bind if another process was recently using it so retry a few times.
    Wait *wait = waitNew(2000);
    int result;

    do
    {
        result = bind(serverSocket, (struct sockaddr *)&address, sizeof(address));
    }
    while (result < 0 && waitMore(wait));

    if (result < 0)
        THROW_SYS_ERROR(AssertError, "unable to bind socket");

    // Listen for client connections
    if (listen(serverSocket, 1) < 0)
        THROW_SYS_ERROR(AssertError, "unable to listen on socket");

    // Loop until no more commands
    TlsSession *serverSession = NULL;
    bool done = false;

    do
    {
        HrnTlsCmd cmd = jsonToUInt(ioReadLine(read));
        const Variant *data = jsonToVar(ioReadLine(read));

        switch (cmd)
        {
            case hrnTlsCmdAbort:
            {
                tlsSessionClose(serverSession, false);
                tlsSessionFree(serverSession);
                serverSession = NULL;

                break;
            }

            case hrnTlsCmdAccept:
            {
                struct sockaddr_in addr;
                unsigned int len = sizeof(addr);

                int testClientSocket = accept(serverSocket, (struct sockaddr *)&addr, &len);

                if (testClientSocket < 0)
                    THROW_SYS_ERROR(AssertError, "unable to accept socket");

                SSL *testClientSSL = SSL_new(serverContext);

                serverSession = tlsSessionNew(
                    testClientSSL, sckSessionNew(sckSessionTypeServer, testClientSocket, STRDEF("client"), 0, 5000), 5000);

                break;
            }

            case hrnTlsCmdClose:
            {
                tlsSessionClose(serverSession, true);
                tlsSessionFree(serverSession);
                serverSession = NULL;

                break;
            }

            case hrnTlsCmdDone:
            {
                done = true;

                break;
            }

            case hrnTlsCmdExpect:
            {
                const String *expected = varStr(data);
                Buffer *buffer = bufNew(strSize(expected));

                ioRead(tlsSessionIoRead(serverSession), buffer);

                // Treat any ? characters as wildcards so variable elements (e.g. auth hashes) can be ignored
                String *actual = strNewBuf(buffer);

                for (unsigned int actualIdx = 0; actualIdx < strSize(actual); actualIdx++)
                {
                    if (strPtr(expected)[actualIdx] == '?')
                        ((char *)strPtr(actual))[actualIdx] = '?';
                }

                // Error if actual does not match expected
                if (!strEq(actual, expected))
                    THROW_FMT(AssertError, "server expected '%s' but got '%s'", strPtr(expected), strPtr(actual));

                break;
            }

            case hrnTlsCmdReply:
            {
                ioWrite(tlsSessionIoWrite(serverSession), BUFSTR(varStr(data)));
                ioWriteFlush(tlsSessionIoWrite(serverSession));

                break;
            }

            case hrnTlsCmdSleep:
            {
                sleepMSec(varUInt64Force(data));

                break;
            }
        }
    }
    while (!done);

    // Free TLS context
    SSL_CTX_free(serverContext);

    FUNCTION_HARNESS_RESULT_VOID();
}

void hrnTlsServerRun(IoRead *read)
{
    FUNCTION_HARNESS_BEGIN();
        FUNCTION_HARNESS_PARAM(IO_READ, read);
    FUNCTION_HARNESS_END();

    if (testContainer())
        hrnTlsServerRunParam(read, STRDEF(TLS_CERT_TEST_CERT), STRDEF(TLS_CERT_TEST_KEY));
    else
    {
        hrnTlsServerRunParam(
            read, strNewFmt("%s/" TEST_CERTIFICATE_PREFIX ".crt", testRepoPath()),
            strNewFmt("%s/" TEST_CERTIFICATE_PREFIX ".key", testRepoPath()));
    }

    FUNCTION_HARNESS_RESULT_VOID();
}

/**********************************************************************************************************************************/
const String *hrnTlsServerHost(void)
{
    return strNew(testContainer() ? TLS_TEST_HOST : "127.0.0.1");
}

/**********************************************************************************************************************************/
unsigned int hrnTlsServerPort(void)
{
    return 44443 + testIdx();
}
