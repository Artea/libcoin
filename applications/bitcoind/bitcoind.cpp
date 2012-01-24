
#include "btcNode/Node.h"
#include "btcNode/NodeRPC.h"

#include "btcHTTP/Server.h"
#include "btcHTTP/Client.h"

#include "btcWallet/wallet.h"
#include "btcWallet/walletrpc.h"

#include "btcMine/Miner.h"
#include "btcMine/MinerRPC.h"

#include <boost/thread.hpp>
#include <boost/program_options.hpp>

#include <fstream>

using namespace std;
using namespace boost;
using namespace boost::program_options;
using namespace json_spirit;

int main(int argc, char* argv[])
{
    string config_file, data_dir;
    unsigned short rpc_port;
    string rpc_bind, rpc_connect, rpc_user, rpc_pass;
    typedef vector<string> strings;
    strings rpc_params;
    strings connect_peers;
    strings add_peers;
    bool gen;

    // Commandline options
    options_description generic("Generic options");
    generic.add_options()
        ("help,?", "Show help messages")
        ("version,v", "print version string")
        ("conf,c", value<string>(&config_file)->default_value("bitcoin.conf"), "Specify configuration file")
        ("datadir", "Specify non default data directory")
    ;
    
    options_description config("Config options");
    config.add_options()
        ("pid", value<string>(), "Specify pid file (default: bitcoind.pid)")
        ("nolisten", "Don't accept connections from outside")
        ("testnet", "Use the test network")
        ("addnode", value<strings>(&add_peers), "Add a node to connect to")
        ("connect", value<strings>(&connect_peers), "Connect only to the specified node")
        ("rpcuser", value<string>(&rpc_user), "Username for JSON-RPC connections")
        ("rpcpassword", value<string>(&rpc_pass), "Password for JSON-RPC connections")
        ("rpcport", value<unsigned short>(&rpc_port)->default_value(8332), "Listen for JSON-RPC connections on <arg>")
        ("rpcallowip", value<string>(&rpc_bind)->default_value(asio::ip::address_v4::loopback().to_string()), "Allow JSON-RPC connections from specified IP address")
        ("rpcconnect", value<string>(&rpc_connect)->default_value(asio::ip::address_v4::loopback().to_string()), "Send commands to node running on <arg>")
        ("keypool", value<unsigned short>(), "Set key pool size to <arg>")
        ("rescan", "Rescan the block chain for missing wallet transactions")
        ("gen", value<bool>(&gen)->default_value(false), "Generate coins")
    ;
    
    options_description hidden("Hidden options");
    hidden.add_options()("params", value<strings>(&rpc_params), "Run JSON RPC command");
    
    options_description cmdline_options;
    cmdline_options.add(generic).add(config).add(hidden);
    
    options_description config_file_options;
    config_file_options.add(config);
    
    options_description visible;
    visible.add(generic).add(config);
    
    positional_options_description pos;
    pos.add("params", -1);
    
    
    // parse the command line
    variables_map args;
    store(command_line_parser(argc, argv).options(cmdline_options).positional(pos).run(), args);
    notify(args);
    
    if (args.count("help")) {
        cout << "Usage: " << argv[0] << " [options] [rpc-command param1 param2 ...]\n";
        cout << visible << "\n";
        cout << "If no rpc-command is specified, " << argv[0] << " start up as a daemon, otherwise it offers commandline access to a running instance\n";
        return 1;
    }
    
    if (args.count("version")) {
        cout << argv[0] << " version is: " << FormatFullVersion() << "\n";
        return 1;        
    }

    if(!args.count("datadir"))
        data_dir = CDB::dataDir(bitcoin.dataDirSuffix());
    
    // if present, parse the config file - if no data dir is specified we always assume bitcoin chain at this stage 
    string config_path = data_dir + "/" + config_file;
    ifstream ifs(config_path.c_str());
    if(ifs) {
        store(parse_config_file(ifs, config_file_options, true), args);
        notify(args);
    }

    Auth auth(rpc_user, rpc_pass); // if rpc_user and rpc_pass are not set all authenticated methods becomes disallowed.
    
    // If we have params on the cmdline we run as a command line client contacting a server
    if (args.count("params")) {
        string rpc_method = rpc_params[0];
        rpc_params.erase(rpc_params.begin());
        // create URL
        string url = "http://" + rpc_connect + ":" + lexical_cast<string>(rpc_port);
        Client client;
        // this is a blocking post!
        Reply reply = client.post(url, RPC::content(rpc_method, rpc_params), auth.headers());
        if(reply.status == Reply::ok) {
            Object rpc_reply = RPC::reply(reply.content);
            Value result = find_value(rpc_reply, "result");
            cout << write_formatted(result) << "\n";
            return 0;
        }
        else {
            cout << "HTTP error code: " << reply.status << "\n";
            Object rpc_reply = RPC::reply(reply.content);
            Object rpc_error = find_value(rpc_reply, "error").get_obj();
            Value code = find_value(rpc_error, "code");
            Value message = find_value(rpc_error, "message");
            cout << "JSON RPC Error code: " << code.get_int() << "\n";
            cout <<  message.get_str() << "\n";
            return 1;
        }
    }
    
    // Else we start the bitcoin node and server!

    const Chain* chain_chooser;
    if(args.count("testnet"))
        chain_chooser = &testnet;
    else
        chain_chooser = &bitcoin;
    const Chain& chain(*chain_chooser);

    if(!args.count("datadir"))
        data_dir = CDB::dataDir(chain.dataDirSuffix());
    
    logfile = data_dir + "/debug.log";
    
    Node node(chain, data_dir, args.count("nolisten") ? "" : "0.0.0.0"); // it is also here we specify the use of a proxy!

    // use the connect and addnode options to restrict and supplement the irc and endpoint db.
    for(strings::iterator ep = add_peers.begin(); ep != add_peers.end(); ++ep) node.addPeer(*ep);
    for(strings::iterator ep = connect_peers.begin(); ep != connect_peers.end(); ++ep) node.connectPeer(*ep);
    
    Wallet wallet(node); // this will also register the needed callbacks
    
    if(args.count("rescan")) {
        wallet.ScanForWalletTransactions();
        printf("Scanned for wallet transactions");
    }
    
    thread nodeThread(&Node::run, &node); // run this as a background thread

    CReserveKey reservekey(&wallet);
    
    Miner miner(node, reservekey);
    miner.setGenerate(gen);
    thread miningThread(&Miner::run, &miner);
    
    Server server(rpc_bind, lexical_cast<string>(rpc_port), filesystem::initial_path().string());
    
    // Register Server methods.
    server.registerMethod(method_ptr(new Stop(server)), auth);
    
    // Register Node methods.
    server.registerMethod(method_ptr(new GetBlockCount(node)));
    server.registerMethod(method_ptr(new GetConnectionCount(node)));
    server.registerMethod(method_ptr(new GetDifficulty(node)));
    server.registerMethod(method_ptr(new GetInfo(node)));
    
    // Register Wallet methods.
    server.registerMethod(method_ptr(new GetBalance(wallet)), auth);
    server.registerMethod(method_ptr(new GetNewAddress(wallet)), auth);
    server.registerMethod(method_ptr(new SendToAddress(wallet)), auth);
    server.registerMethod(method_ptr(new GetAccountAddress(wallet)), auth);
    server.registerMethod(method_ptr(new GetAccount(wallet)), auth);
    server.registerMethod(method_ptr(new SetAccount(wallet)), auth);
    server.registerMethod(method_ptr(new GetAddressesByAccount(wallet)), auth);
    server.registerMethod(method_ptr(new SetTxFee(wallet)), auth);
    server.registerMethod(method_ptr(new GetReceivedByAddress(wallet)), auth);
    server.registerMethod(method_ptr(new GetReceivedByAccount(wallet)), auth);
    server.registerMethod(method_ptr(new MoveCmd(wallet)), auth);
    server.registerMethod(method_ptr(new SendFrom(wallet)), auth);
    server.registerMethod(method_ptr(new SendMany(wallet)), auth);
    server.registerMethod(method_ptr(new ListReceivedByAddress(wallet)), auth);
    server.registerMethod(method_ptr(new ListReceivedByAccount(wallet)), auth);
    server.registerMethod(method_ptr(new ListTransactions(wallet)), auth);
    server.registerMethod(method_ptr(new ListAccounts(wallet)), auth);
    server.registerMethod(method_ptr(new GetTransaction(wallet)), auth);
    server.registerMethod(method_ptr(new BackupWallet(wallet)), auth);
    server.registerMethod(method_ptr(new KeypoolRefill(wallet)), auth);
    server.registerMethod(method_ptr(new WalletPassphraseChange(wallet)), auth);
    server.registerMethod(method_ptr(new WalletLock(wallet)), auth);
    server.registerMethod(method_ptr(new EncryptWallet(wallet)), auth);
    server.registerMethod(method_ptr(new ValidateAddress(wallet)), auth);
    // this method also takes the io_service from the server to start a deadline timer locking the wallet again.
    server.registerMethod(method_ptr(new WalletPassphrase(wallet, server.get_io_service())), auth);
    
    // Register Mining methods.
    server.registerMethod(method_ptr(new SetGenerate(miner)), auth);    
    server.registerMethod(method_ptr(new GetGenerate(miner)), auth);    
    server.registerMethod(method_ptr(new GetHashesPerSec(miner)), auth);    
    
    server.run();

    printf("Server exitted, shutting down Node and Miner...\n");
    // getting here means that we have exited from the server (e.g. by the quit method)
    
    miner.shutdown();
    miningThread.join();
    
    node.shutdown();
    nodeThread.join();
    
    return 0;    
}