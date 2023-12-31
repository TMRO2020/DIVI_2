// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2009-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparamsbase.h"
#include "clientversion.h"
#include <DataDirectory.h>
#include <LicenseAndInfo.h>
#include "rpcclient.h"
#include "rpcprotocol.h"
#include "util.h"
#include "utilstrencodings.h"
#include "Settings.h"
#include <ThreadManagementHelpers.h>

#include <boost/filesystem/operations.hpp>

#define translate(x) std::string(x) /* Keep the translate() around in case gettext or such will be used later to translate non-UI */

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace json_spirit;
extern Settings& settings;

//////////////////////////////////////////////////////////////////////////////
//
// Start
//

//
// Exception thrown on connection error.  This error is used to determine
// when to wait if -rpcwait is given.
//
class CConnectionFailed : public std::runtime_error
{
public:
    explicit inline CConnectionFailed(const std::string& msg) : std::runtime_error(msg)
    {
    }
};

static bool AppInitRPC(int argc, char* argv[])
{
    //
    // Parameters
    //
    settings.ParseParameters(argc, argv);
    if (argc < 2 ||settings.ParameterIsSet("-?") ||settings.ParameterIsSet("-help") ||settings.ParameterIsSet("-version")) {
        std::string strUsage = translate("Divi Core RPC client version") + " " + FormatFullVersion() + "\n";
        if (settings.ParameterIsSet("-version")) {
            strUsage += LicenseInfo();
            strUsage += "\n" + translate("Usage:") + "\n" +
                        "  divi-cli [options] <command> [params]  " + translate("Send command to Divi Core") + "\n" +
                        "  divi-cli [options] help                " + translate("List commands") + "\n" +
                        "  divi-cli [options] help <command>      " + translate("Get help for a command") + "\n";

            strUsage += "\n" + HelpMessageCli();
        }

        fprintf(stdout, "%s", strUsage.c_str());
        return false;
    }
    if (!boost::filesystem::is_directory(GetDataDir(false))) {
        fprintf(stderr, "Error: Specified data directory \"%s\" does not exist.\n", settings.GetParameter("-datadir").c_str());
        return false;
    }
    try {
        settings.ReadConfigFile();
    } catch (std::exception& e) {
        fprintf(stderr, "Error reading configuration file: %s\n", e.what());
        return false;
    }
    // Check for -testnet or -regtest parameter (BaseParams() calls are only valid after this clause)
    if (!SelectBaseParamsFromCommandLine(settings)) {
        fprintf(stderr, "Error: Invalid combination of -regtest and -testnet.\n");
        return false;
    }
    return true;
}

Object CallRPC(const string& strMethod, const Array& params)
{
    if (settings.GetParameter("-rpcuser") == "" && settings.GetParameter("-rpcpassword") == "")
        throw runtime_error(strprintf(
            translate("You must set rpcpassword=<password> in the configuration file:\n%s\n"
              "If the file does not exist, create it with owner-readable-only file permissions."),
            settings.GetConfigFile().string().c_str()));

    // Connect to localhost
    basic_socket_iostream<ip::tcp> stream;
    stream.connect(settings.GetArg("-rpcconnect", "127.0.0.1"), settings.GetArg("-rpcport", itostr(BaseParams().RPCPort())));
    if (!stream)
        throw CConnectionFailed("couldn't connect to server");

    // HTTP basic authentication
    string strUserPass64 = EncodeBase64(settings.GetParameter("-rpcuser") + ":" + settings.GetParameter("-rpcpassword"));
    map<string, string> mapRequestHeaders;
    mapRequestHeaders["Authorization"] = string("Basic ") + strUserPass64;

    // Send request
    string strRequest = JSONRPCRequest(strMethod, params, 1);
    string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive HTTP reply status
    int nProto = 0;
    int nStatus = ReadHTTPStatus(stream, nProto);

    // Receive HTTP reply message headers and body
    map<string, string> mapHeaders;
    string strReply;
    ReadHTTPMessage(stream, mapHeaders, strReply, nProto, std::numeric_limits<size_t>::max());

    if (nStatus == HTTP_UNAUTHORIZED)
        throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != HTTP_BAD_REQUEST && nStatus != HTTP_NOT_FOUND && nStatus != HTTP_INTERNAL_SERVER_ERROR)
        throw runtime_error(strprintf("server returned HTTP error %d", nStatus));
    else if (strReply.empty())
        throw runtime_error("no response from server");

    // Parse reply
    Value valReply;
    if (!read_string(strReply, valReply))
        throw runtime_error("couldn't parse reply from server");
    const Object& reply = valReply.get_obj();
    if (reply.empty())
        throw runtime_error("expected reply to have result, error and id properties");

    return reply;
}

int CommandLineRPC(int argc, char* argv[])
{
    string strPrint;
    int nRet = 0;
    try {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0])) {
            argc--;
            argv++;
        }

        // Method
        if (argc < 2)
            throw runtime_error("too few parameters");
        string strMethod = argv[1];

        // Parameters default to strings
        std::vector<std::string> strParams(&argv[2], &argv[argc]);
        Array params = RPCConvertValues(strMethod, strParams);

        // Execute and handle connection failures with -rpcwait
        const bool fWait = settings.GetBoolArg("-rpcwait", false);
        do {
            try {
                const Object reply = CallRPC(strMethod, params);

                // Parse reply
                const Value& result = find_value(reply, "result");
                const Value& error = find_value(reply, "error");

                if (error.type() != null_type) {
                    // Error
                    const int code = find_value(error.get_obj(), "code").get_int();
                    if (fWait && code == RPC_IN_WARMUP)
                        throw CConnectionFailed("server in warmup");
                    strPrint = "error: " + write_string(error, false);
                    nRet = abs(code);
                } else {
                    // Result
                    if (result.type() == null_type)
                        strPrint = "";
                    else if (result.type() == str_type)
                        strPrint = result.get_str();
                    else
                        strPrint = write_string(result, true);
                }

                // Connection succeeded, no need to retry.
                break;
            } catch (const CConnectionFailed& e) {
                if (fWait)
                    MilliSleep(1000);
                else
                    throw;
            }
        } while (fWait);
    } catch (boost::thread_interrupted) {
        throw;
    } catch (std::exception& e) {
        strPrint = string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRPC()");
        throw;
    }

    if (strPrint != "") {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }
    return nRet;
}

int main(int argc, char* argv[])
{
    SetupEnvironment();

    try {
        if (!AppInitRPC(argc, argv))
            return EXIT_FAILURE;
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "AppInitRPC()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "AppInitRPC()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRPC(argc, argv);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRPC()");
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRPC()");
    }
    return ret;
}
