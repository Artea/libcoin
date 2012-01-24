#include "btcNode/BlockChain.h"

#include "btc/Block.h"
#include "btcNode/BlockIndex.h"
#include "btcNode/MessageHeader.h"

#include "btcNode/Peer.h"

#include "btc/script.h"

using namespace std;
using namespace boost;

//
// BlockChain
//

BlockChain::BlockChain(const Chain& chain, const string dataDir, const char* pszMode) : CDB(dataDir == "" ? CDB::dataDir(chain.dataDirSuffix()) : dataDir, "blkindex.dat", pszMode), _chain(chain), _blockFile(dataDir == "" ? CDB::dataDir(chain.dataDirSuffix()) : dataDir), _genesisBlockIndex(NULL), _bestChainWork(0), _bestInvalidWork(0), _bestChain(0), _bestIndex(NULL), _bestReceivedTime(0), _transactionsUpdated(0) {
    load();
}


bool BlockChain::load(bool allowNew)
{    
    //
    // Load block index
    //
    if (!LoadBlockIndex())
        return false;
    
    //
    // Init with genesis block
    //
    if (_blockChainIndex.empty()) {
        if (!allowNew)
            return false;
        
        // Start new block file
        
        unsigned int nFile;
        unsigned int nBlockPos;
        Block block(_chain.genesisBlock());
        if (!_blockFile.writeToDisk(_chain, block, nFile, nBlockPos, true))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!addToBlockIndex(block, nFile, nBlockPos))
            return error("LoadBlockIndex() : genesis block not accepted");
        }
    
    return true;
}

void BlockChain::print()
{
    // precompute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (BlockChainIndex::iterator mi = _blockChainIndex.begin(); mi != _blockChainIndex.end(); ++mi)
        {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
        }
    
    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, _genesisBlockIndex));
    
    int nPrevCol = 0;
    while (!vStack.empty())
        {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();
        
        // print split or gap
        if (nCol > nPrevCol)
            {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
            }
        else if (nCol < nPrevCol)
            {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
            }
        nPrevCol = nCol;
        
        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");
        
        // print item
        Block block;
        _blockFile.readFromDisk(block, pindex);
        printf("%d (%u,%u) %s  %s  tx %d",
               pindex->nHeight,
               pindex->nFile,
               pindex->nBlockPos,
               block.getHash().toString().substr(0,20).c_str(),
               DateTimeStrFormat("%x %H:%M:%S", block.getBlockTime()).c_str(),
               block.getNumTransactions());
        
        //        PrintWallets(block);
        
        // put the main timechain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (int i = 0; i < vNext.size(); i++)
            {
            if (vNext[i]->pnext)
                {
                swap(vNext[0], vNext[i]);
                break;
                }
            }
        
        // iterate children
        for (int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
        }
}

CBlockLocator BlockChain::getBestLocator() const {
    CBlockLocator l;
    const CBlockIndex* pindex = getBestIndex();
    l.vHave.clear();
    int nStep = 1;
    while (pindex) {
        l.vHave.push_back(pindex->GetBlockHash());
        
        // Exponentially larger steps back
        for (int i = 0; pindex && i < nStep; i++)
            pindex = pindex->pprev;
        if (l.vHave.size() > 10)
            nStep *= 2;
        }
    l.vHave.push_back(getGenesisHash());
    return l;
}

bool BlockChain::isInitialBlockDownload()
{
    const int initialBlockThreshold = 120; // Regard blocks up until N-threshold as "initial download"

    if (_bestIndex == NULL || getBestHeight() < (getTotalBlocksEstimate()-initialBlockThreshold))
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (_bestIndex != pindexLastBest)
        {
        pindexLastBest = _bestIndex;
        nLastUpdate = GetTime();
        }
    return (GetTime() - nLastUpdate < 10 &&
            _bestIndex->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

// BlockLocator interface
/*
CBlockLocator BlockChain::blockLocator(uint256 hashBlock)
{
    CBlockLocator locator;
    BlockChainIndex::iterator mi = _blockChainIndex.find(hashBlock);
    if (mi != _blockChainIndex.end())
        locator.Set((*mi).second);
    return locator;
}
*/
int BlockChain::getDistanceBack(const CBlockLocator& locator) const
{
    // Retrace how far back it was in the sender's branch
    int nDistance = 0;
    int nStep = 1;
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
        BlockChainIndex::const_iterator mi = _blockChainIndex.find(hash);
        if (mi != _blockChainIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (isInMainChain(pindex))
                return nDistance;
        }
        nDistance += nStep;
        if (nDistance > 10)
            nStep *= 2;
    }
    return nDistance;
}

void BlockChain::getBlock(const uint256 hash, Block& block) const
{
    block.setNull();
    BlockChainIndex::const_iterator index = _blockChainIndex.find(hash);
    if (index != _blockChainIndex.end()) {
        _blockFile.readFromDisk(block, index->second);
    }
    // now try if the hash was for a transaction
    TxIndex txidx;
    if(ReadTxIndex(hash, txidx)) {
        _blockFile.readFromDisk(block, txidx.getPos().getFile(), txidx.getPos().getBlockPos());
    }
}

void BlockChain::getBlock(const CBlockIndex* index, Block& block) const {
    _blockFile.readFromDisk(block, index);
}



const CBlockIndex* BlockChain::getBlockIndex(const CBlockLocator& locator) const
{
    // Find the first block the caller has in the main chain
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
        BlockChainIndex::const_iterator mi = _blockChainIndex.find(hash);
        if (mi != _blockChainIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (isInMainChain(pindex))
                return pindex;
        }
    }
    return _genesisBlockIndex;
}

const CBlockIndex* BlockChain::getBlockIndex(const uint256 hash) const
{
    BlockChainIndex::const_iterator index = _blockChainIndex.find(hash);
    if (index != _blockChainIndex.end())
        return index->second;
    else
        return NULL;
}


uint256 BlockChain::getBlockHash(const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
    BlockChainIndex::iterator mi = _blockChainIndex.find(hash);
    if (mi != _blockChainIndex.end())
        {
        CBlockIndex* pindex = (*mi).second;
        if (isInMainChain(pindex))
            return hash;
        }
    }
    return getGenesisHash();
}

CBlockIndex* BlockChain::getHashStopIndex(uint256 hashStop)
{
    BlockChainIndex::iterator mi = _blockChainIndex.find(hashStop);
    if (mi == _blockChainIndex.end())
        return NULL;
    return (*mi).second;
}

bool BlockChain::connectInputs(const Transaction& tx, map<uint256, TxIndex>& mapTestPool, DiskTxPos posThisTx,
                   const CBlockIndex* pindexBlock, int64& nFees, bool fBlock, bool fMiner, int64 nMinFee) const
{
    // Take over previous transactions' spent pointers
    if (!tx.IsCoinBase()) {
        int64 nValueIn = 0;
        for (int i = 0; i < tx.vin.size(); i++) {
            Coin prevout = tx.vin[i].prevout;
            
            // Read txindex
            TxIndex txindex;
            bool fFound = true;
            if ((fBlock || fMiner) && mapTestPool.count(prevout.hash)) {
                // Get txindex from current proposed changes
                txindex = mapTestPool[prevout.hash];
            }
            else {
                // Read txindex from txdb
                fFound = ReadTxIndex(prevout.hash, txindex);
            }
            if (!fFound && (fBlock || fMiner))
                return fMiner ? false : error("ConnectInputs() : %s prev tx %s index entry not found", tx.GetHash().toString().substr(0,10).c_str(),  prevout.hash.toString().substr(0,10).c_str());
            
            // Read txPrev
            Transaction txPrev;
            if (!fFound || txindex.getPos() == DiskTxPos(1,1,1)) {
                // Get prev tx from single transactions in memory
                TransactionIndex::const_iterator index = _transactionIndex.find(prevout.hash);
                if (index == _transactionIndex.end())
                    return error("ConnectInputs() : %s mapTransactions prev not found %s", tx.GetHash().toString().substr(0,10).c_str(),  prevout.hash.toString().substr(0,10).c_str());
                txPrev = index->second;
                if (!fFound)
                    txindex.resizeSpents(txPrev.vout.size());
            }
            else {
                // Get prev tx from disk
                if (!_blockFile.readFromDisk(txPrev, txindex.getPos()))
                    return error("ConnectInputs() : %s ReadFromDisk prev tx %s failed", tx.GetHash().toString().substr(0,10).c_str(),  prevout.hash.toString().substr(0,10).c_str());
                }
            
            if (prevout.index >= txPrev.vout.size() || prevout.index >= txindex.getNumSpents())
                return error("ConnectInputs() : %s prevout.n out of range %d %d %d prev tx %s\n%s", tx.GetHash().toString().substr(0,10).c_str(), prevout.index, txPrev.vout.size(), txindex.getNumSpents(), prevout.hash.toString().substr(0,10).c_str(), txPrev.toString().c_str());
            
            // If prev is coinbase, check that it's matured
            if (txPrev.IsCoinBase())
                for (const CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < COINBASE_MATURITY; pindex = pindex->pprev)
                    if (pindex->nBlockPos == txindex.getPos().getBlockPos() && pindex->nFile == txindex.getPos().getFile())
                        return error("ConnectInputs() : tried to spend coinbase at depth %d", pindexBlock->nHeight - pindex->nHeight);
            
            // Verify signature
            if (!VerifySignature(txPrev, tx, i))
                return error("ConnectInputs() : %s VerifySignature failed", tx.GetHash().toString().substr(0,10).c_str());
            
            // Check for conflicts
            if (!txindex.getSpent(prevout.index).isNull())
                return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", tx.GetHash().toString().substr(0,10).c_str(), txindex.getSpent(prevout.index).toString().c_str());
            
            // Check for negative or overflow input values
            nValueIn += txPrev.vout[prevout.index].nValue;
            if (!MoneyRange(txPrev.vout[prevout.index].nValue) || !MoneyRange(nValueIn))
                return error("ConnectInputs() : txin values out of range");
            
            // Mark outpoints as spent
            txindex.setSpent(prevout.index, posThisTx);
            
            // Write back
            if (fBlock || fMiner) {
                mapTestPool[prevout.hash] = txindex;
            }
        }
        
        if (nValueIn < tx.GetValueOut())
            return error("ConnectInputs() : %s value in < value out", tx.GetHash().toString().substr(0,10).c_str());
        
        // Tally transaction fees
        int64 nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return error("ConnectInputs() : %s nTxFee < 0", tx.GetHash().toString().substr(0,10).c_str());
        if (nTxFee < nMinFee)
            return false;
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return error("ConnectInputs() : nFees out of range");
        }
    
    if (fBlock) {
        // Add transaction to changes
        mapTestPool[tx.GetHash()] = TxIndex(posThisTx, tx.vout.size());
    }
    else if (fMiner) {
        // Add transaction to test pool
        mapTestPool[tx.GetHash()] = TxIndex(DiskTxPos(1,1,1), tx.vout.size());
    }
    
    return true;
}

bool BlockChain::disconnectInputs(Transaction& tx)
{
    // Relinquish previous transactions' spent pointers
    if (!tx.IsCoinBase()) {
        BOOST_FOREACH(const CTxIn& txin, tx.vin) {
            Coin prevout = txin.prevout;
            
            // Get prev txindex from disk
            TxIndex txindex;
            if (!ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");
            
            if (prevout.index >= txindex.getNumSpents())
                return error("DisconnectInputs() : prevout.n out of range");
            
            // Mark outpoint as not spent
            txindex.setNotSpent(prevout.index);
            
            // Write back
            if (!UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
            }
        }
    
    // Remove transaction from index
    if (!EraseTxIndex(tx))
        return error("DisconnectInputs() : EraseTxPos failed");
    
    return true;
}

bool BlockChain::isInMainChain(const uint256 hash) const {
    BlockChainIndex::const_iterator i = _blockChainIndex.find(hash);
    return i != _blockChainIndex.end();
}

int BlockChain::getHeight(const uint256 hash) const
{
    // first check if this is a block hash
    BlockChainIndex::const_iterator i = _blockChainIndex.find(hash);
    if (i != _blockChainIndex.end()) {
        return i->second->nHeight;
    }
    
    // assume it is a tx
    
    TxIndex txindex;
    if(!ReadTxIndex(hash, txindex))
        return -1;
    
    // Read block header
    Block block;
    if (!_blockFile.readFromDisk(block, txindex.getPos().getFile(), txindex.getPos().getBlockPos(), false))
        return -1;
    // Find the block in the index
    BlockChainIndex::const_iterator mi = _blockChainIndex.find(block.getHash());
    if (mi == _blockChainIndex.end())
        return -1;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !isInMainChain(pindex))
        return -1;
    return pindex->nHeight;
}

// Private interface

bool BlockChain::ReadTxIndex(uint256 hash, TxIndex& txindex) const
{
    txindex.setNull();
    return Read(make_pair(string("tx"), hash), txindex);
}

bool BlockChain::readDrIndex(uint160 hash160, set<Coin>& debit) const
{
    //    txindex.SetNull();
    return Read(make_pair(string("dr"), ChainAddress(_chain.networkId(), hash160).toString()), debit);
}

bool BlockChain::readCrIndex(uint160 hash160, set<Coin>& credit) const
{
    //    txindex.SetNull();
    return Read(make_pair(string("cr"), ChainAddress(_chain.networkId(), hash160).toString()), credit);
}

bool Solver(const CScript& scriptPubKey, vector<pair<opcodetype, vector<unsigned char> > >& vSolutionRet);

bool BlockChain::UpdateTxIndex(uint256 hash, const TxIndex& txindex)
{
    
    //    cout << "update tx: " << hash.toString() << endl;    
    /*
    Transaction tx;
    _blockFile.readFromDisk(tx, txindex.getPos());
    
    // gronager: hook to enable public key / hash160 lookups by a separate database
    // first find the keys and hash160s that are referenced in this transaction
    typedef pair<uint160, unsigned int> AssetPair;
    vector<AssetPair> vDebit;
    vector<AssetPair> vCredit;
    
    // for each tx out in the newly added tx check for a pubkey or a pubkeyhash in the script
    for(unsigned int n = 0; n < tx.vout.size(); n++)
        {
        const CTxOut& txout = tx.vout[n];
        vector<pair<opcodetype, vector<unsigned char> > > vSolution;
        if (!Solver(txout.scriptPubKey, vSolution))
            break;
        
        BOOST_FOREACH(PAIRTYPE(opcodetype, vector<unsigned char>)& item, vSolution)
            {
            vector<unsigned char> vchPubKey;
            if (item.first == OP_PUBKEY)
                {
                // encode the pubkey into a hash160
                vDebit.push_back(AssetPair(Hash160(item.second), n));                
                }
            else if (item.first == OP_PUBKEYHASH)
                {
                vDebit.push_back(AssetPair(uint160(item.second), n));                
                }
            }
        }
    if(!tx.IsCoinBase())
        {
        for(unsigned int n = 0; n < tx.vin.size(); n++)
            {
            const CTxIn& txin = tx.vin[n];
            Transaction prevtx;
            if(!ReadDiskTx(txin.prevout, prevtx))
                continue; // OK ???
            CTxOut txout = prevtx.vout[txin.prevout.n];        
            
            vector<pair<opcodetype, vector<unsigned char> > > vSolution;
            if (!Solver(txout.scriptPubKey, vSolution))
                break;
            
            BOOST_FOREACH(PAIRTYPE(opcodetype, vector<unsigned char>)& item, vSolution)
                {
                vector<unsigned char> vchPubKey;
                if (item.first == OP_PUBKEY)
                    {
                    // encode the pubkey into a hash160
                    vCredit.push_back(pair<uint160, unsigned int>(Hash160(item.second), n));                
                    }
                else if (item.first == OP_PUBKEYHASH)
                    {
                    vCredit.push_back(pair<uint160, unsigned int>(uint160(item.second), n));                
                    }
                }
            }
        }
    
    for(vector<AssetPair>::iterator hashpair = vDebit.begin(); hashpair != vDebit.end(); ++hashpair)
        {
        set<Coin> txhashes;
        Read(make_pair(string("dr"), ChainAddress(hashpair->first).toString()), txhashes);
        //        cout << "\t debit: " << ChainAddress(hashpair->first).toString() << endl;    
        txhashes.insert(Coin(hash, hashpair->second));
        Write(make_pair(string("dr"), ChainAddress(hashpair->first).toString()), txhashes); // overwrite!
        }
    
    for(vector<AssetPair>::iterator hashpair = vCredit.begin(); hashpair != vCredit.end(); ++hashpair)
        {
        set<Coin> txhashes;
        Read(make_pair(string("cr"), ChainAddress(hashpair->first).toString()), txhashes);
        //        cout << "\t credit: " << ChainAddress(hashpair->first).toString() << endl;    
        txhashes.insert(Coin(hash, hashpair->second));
        Write(make_pair(string("cr"), ChainAddress(hashpair->first).toString()), txhashes); // overwrite!
        }
    //    cout << "and write tx" << std::endl;
     */
    return Write(make_pair(string("tx"), hash), txindex);
}

bool BlockChain::AddTxIndex(const Transaction& tx, const DiskTxPos& pos, int nHeight)
{
    // Add to tx index
    uint256 hash = tx.GetHash();
    TxIndex txindex(pos, tx.vout.size());
    
    return UpdateTxIndex(hash, txindex);
    //    return Write(make_pair(string("tx"), hash), txindex);
}


bool BlockChain::EraseTxIndex(const Transaction& tx)
{
    uint256 hash = tx.GetHash();
/*    
    // gronager: hook to enable public key / hash160 lookups by a separate database
    // first find the keys and hash160s that are referenced in this transaction
    typedef pair<uint160, unsigned int> AssetPair;
    vector<AssetPair> vDebit;
    vector<AssetPair> vCredit;
    cout << "erase tx: " << hash.toString() << endl;    
    // for each tx out in the newly added tx check for a pubkey or a pubkeyhash in the script
    for(unsigned int n = 0; n < tx.vout.size(); n++)
        {
        const CTxOut& txout = tx.vout[n];
        vector<pair<opcodetype, vector<unsigned char> > > vSolution;
        if (!Solver(txout.scriptPubKey, vSolution))
            break;
        
        BOOST_FOREACH(PAIRTYPE(opcodetype, vector<unsigned char>)& item, vSolution)
            {
            vector<unsigned char> vchPubKey;
            if (item.first == OP_PUBKEY)
                {
                // encode the pubkey into a hash160
                vDebit.push_back(AssetPair(Hash160(item.second), n));                
                }
            else if (item.first == OP_PUBKEYHASH)
                {
                vDebit.push_back(AssetPair(uint160(item.second), n));                
                }
            }
        }
    if(!tx.IsCoinBase())
        {
        for(unsigned int n = 0; n < tx.vin.size(); n++)
            {
            const CTxIn& txin = tx.vin[n];
            Transaction prevtx;
            if(!ReadDiskTx(txin.prevout, prevtx))
                continue; // OK ???
            CTxOut txout = prevtx.vout[txin.prevout.n];        
            
            vector<pair<opcodetype, vector<unsigned char> > > vSolution;
            if (!Solver(txout.scriptPubKey, vSolution))
                break;
            
            BOOST_FOREACH(PAIRTYPE(opcodetype, vector<unsigned char>)& item, vSolution)
                {
                vector<unsigned char> vchPubKey;
                if (item.first == OP_PUBKEY)
                    {
                    // encode the pubkey into a hash160
                    vCredit.push_back(pair<uint160, unsigned int>(Hash160(item.second), n));                
                    }
                else if (item.first == OP_PUBKEYHASH)
                    {
                    vCredit.push_back(pair<uint160, unsigned int>(uint160(item.second), n));                
                    }
                }
            }
        }
    
    for(vector<AssetPair>::iterator hashpair = vDebit.begin(); hashpair != vDebit.end(); ++hashpair)
        {
        set<Coin> txhashes;
        Read(make_pair(string("dr"), ChainAddress(hashpair->first).toString()), txhashes);
        txhashes.erase(Coin(hash, hashpair->second));
        Write(make_pair(string("dr"), ChainAddress(hashpair->first).toString()), txhashes); // overwrite!
        }
    
    for(vector<AssetPair>::iterator hashpair = vCredit.begin(); hashpair != vCredit.end(); ++hashpair)
        {
        set<Coin> txhashes;
        Read(make_pair(string("cr"), ChainAddress(hashpair->first).toString()), txhashes);
        txhashes.erase(Coin(hash, hashpair->second));
        Write(make_pair(string("cr"), ChainAddress(hashpair->first).toString()), txhashes); // overwrite!
        }
*/    
    return Erase(make_pair(string("tx"), hash));
}

bool BlockChain::haveTx(uint256 hash, bool must_be_confirmed) const
{
    if(Exists(make_pair(string("tx"), hash)))
        return true;
    else if(!must_be_confirmed && _transactionIndex.count(hash))
        return true;
    else
        return false;
}

bool BlockChain::isFinal(const Transaction& tx, int nBlockHeight, int64 nBlockTime) const
{
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = getBestHeight();
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64)tx.nLockTime < (tx.nLockTime < LOCKTIME_THRESHOLD ? (int64)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

bool BlockChain::haveBlock(uint256 hash) const
{
    return _blockChainIndex.count(hash);
}

bool BlockChain::ReadOwnerTxes(uint160 hash160, int nMinHeight, vector<Transaction>& vtx)
{
    vtx.clear();
    
    // Get cursor
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;
    
    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
    // Read next record
    CDataStream ssKey;
    if (fFlags == DB_SET_RANGE)
        ssKey << string("owner") << hash160 << DiskTxPos(0, 0, 0);
    CDataStream ssValue;
    int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
    fFlags = DB_NEXT;
    if (ret == DB_NOTFOUND)
        break;
    else if (ret != 0)
        {
        pcursor->close();
        return false;
        }
    
    // Unserialize
    string strType;
    uint160 hashItem;
    DiskTxPos pos;
    ssKey >> strType >> hashItem >> pos;
    int nItemHeight;
    ssValue >> nItemHeight;
    
    // Read transaction
    if (strType != "owner" || hashItem != hash160)
        break;
    if (nItemHeight >= nMinHeight)
        {
        vtx.resize(vtx.size()+1);
        if (!_blockFile.readFromDisk(vtx.back(), pos))
            {
            pcursor->close();
            return false;
            }
        }
    }
    
    pcursor->close();
    return true;
}

bool BlockChain::ReadDiskTx(uint256 hash, Transaction& tx, TxIndex& txindex) const
{
    tx.SetNull();
    if (!ReadTxIndex(hash, txindex))
        return false;
    return (_blockFile.readFromDisk(tx, txindex.getPos()));
}

bool BlockChain::readDiskTx(uint256 hash, Transaction& tx) const
{
    TxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool BlockChain::readDiskTx(uint256 hash, Transaction& tx, int64& height, int64& time) const
{
    TxIndex txindex;
    if(!ReadDiskTx(hash, tx, txindex)) return false;
    
    // Read block header
    Block block;
    if (!_blockFile.readFromDisk(block, txindex.getPos().getFile(), txindex.getPos().getBlockPos(), false))
        return false;
    // Find the block in the index
    BlockChainIndex::const_iterator mi = _blockChainIndex.find(block.getHash());
    if (mi == _blockChainIndex.end())
        return false;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !isInMainChain(pindex))
        return false;

    height = pindex->nHeight;
    time = pindex->nTime;
    
    return true;
}


bool BlockChain::ReadDiskTx(Coin outpoint, Transaction& tx, TxIndex& txindex) const
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool BlockChain::ReadDiskTx(Coin outpoint, Transaction& tx) const
{
    TxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool BlockChain::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair(string("blockindex"), blockindex.GetBlockHash()), blockindex);
}

bool BlockChain::EraseBlockIndex(uint256 hash)
{
    return Erase(make_pair(string("blockindex"), hash));
}

bool BlockChain::ReadHashBestChain()
{
    return Read(string("hashBestChain"), _bestChain);
}

bool BlockChain::WriteHashBestChain()
{
    return Write(string("hashBestChain"), _bestChain);
}

bool BlockChain::ReadBestInvalidWork()
{
    return Read(string("bnBestInvalidWork"), _bestInvalidWork);
}

bool BlockChain::WriteBestInvalidWork()
{
    return Write(string("bnBestInvalidWork"), _bestInvalidWork);
}

CBlockIndex* BlockChain::InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;
    
    // Return existing
    BlockChainIndex::iterator mi = _blockChainIndex.find(hash);
    if (mi != _blockChainIndex.end())
        return (*mi).second;
    
    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = _blockChainIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    
    return pindexNew;
}

bool BlockChain::LoadBlockIndex()
{
    // Get database cursor
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;
    
    // Load _blockChainIndex
    unsigned int fFlags = DB_SET_RANGE;
    loop {
        // Read next record
        CDataStream ssKey;
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("blockindex"), uint256(0));
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue, fFlags);
        fFlags = DB_NEXT;
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;
        
        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "blockindex") {
            CDiskBlockIndex diskindex;
            ssValue >> diskindex;
            
            // Construct block index object
            CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
            pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
            pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
            pindexNew->nFile          = diskindex.nFile;
            pindexNew->nBlockPos      = diskindex.nBlockPos;
            pindexNew->nHeight        = diskindex.nHeight;
            pindexNew->nVersion       = diskindex.nVersion;
            pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
            pindexNew->nTime          = diskindex.nTime;
            pindexNew->nBits          = diskindex.nBits;
            pindexNew->nNonce         = diskindex.nNonce;
            
            // Watch for genesis block
            if (_genesisBlockIndex == NULL && diskindex.GetBlockHash() == getGenesisHash())
                _genesisBlockIndex = pindexNew;
            
            if (!pindexNew->checkIndex(_chain.proofOfWorkLimit()))
                return error("LoadBlockIndex() : CheckIndex failed at %d", pindexNew->nHeight);
            //            if (!pindexNew->CheckIndex())
            //                return error("LoadBlockIndex() : CheckIndex failed at %d", pindexNew->nHeight);
        }
        else {
            break;
        }
    }
    pcursor->close();
    
    // Calculate bnChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(_blockChainIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, _blockChainIndex)
    {
    CBlockIndex* pindex = item.second;
    vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
    CBlockIndex* pindex = item.second;
    pindex->bnChainWork = (pindex->pprev ? pindex->pprev->bnChainWork : 0) + pindex->GetBlockWork();
    }
    
    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain()) {
        if (_genesisBlockIndex == NULL)
            return true;
        return error("BlockChain::LoadBlockIndex() : hashBestChain not loaded");
    }
    if (!_blockChainIndex.count(_bestChain))
        return error("BlockChain::LoadBlockIndex() : hashBestChain not found in the block index");
    _bestIndex = _blockChainIndex[_bestChain];
    _bestChainWork = _bestIndex->bnChainWork;
    printf("LoadBlockIndex(): hashBestChain=%s  height=%d\n", _bestChain.toString().substr(0,20).c_str(), getBestHeight());
    
    // Load _bestChainWork, OK if it doesn't exist
    ReadBestInvalidWork();
    
    // Verify blocks in the best chain
    CBlockIndex* pindexFork = NULL;
    for (CBlockIndex* pindex = _bestIndex; pindex && pindex->pprev; pindex = pindex->pprev)
        {
        if (pindex->nHeight < getBestHeight()-2500 && !mapArgs.count("-checkblocks"))
            break;
        Block block;
        if (!_blockFile.readFromDisk(block, pindex))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        if (!block.checkBlock(_chain.proofOfWorkLimit()))
            {
            printf("LoadBlockIndex() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().toString().c_str());
            pindexFork = pindex->pprev;
            }
        }
    if (pindexFork) {
        // Reorg back to the fork
        printf("LoadBlockIndex() : *** moving best chain pointer back to block %d\n", pindexFork->nHeight);
        Block block;
        if (!_blockFile.readFromDisk(block, pindexFork))
            return error("LoadBlockIndex() : block.ReadFromDisk failed");
        setBestChain(block, pindexFork);
    }
    
    return true;
}

bool BlockChain::disconnectBlock(Block& block, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = block.getNumTransactions()-1; i >= 0; i--)
        if (!disconnectInputs(block.getTransaction(i)))
            return false;
    
    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev) {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!WriteBlockIndex(blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }
    
    return true;
}

bool BlockChain::connectBlock(Block& block, CBlockIndex* pindex)
{
    // Check it again in case a previous version let a bad block in
    if (!block.checkBlock(_chain.proofOfWorkLimit()))
        return false;
    
    //// issue here: it doesn't know the version
    unsigned int nTxPos = pindex->nBlockPos + ::GetSerializeSize(Block(), SER_DISK) - 1 + GetSizeOfCompactSize(block.getNumTransactions());
    
    map<uint256, TxIndex> queuedChanges;
    int64 fees = 0;
    for(int i = 0; i < block.getNumTransactions(); ++i) {
        Transaction& tx = block.getTransaction(i);
        DiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        nTxPos += ::GetSerializeSize(tx, SER_DISK);
        
        if (!connectInputs(tx, queuedChanges, posThisTx, pindex, fees, true, false))
            return false;
    }
    // Write queued txindex changes
    for (map<uint256, TxIndex>::iterator mi = queuedChanges.begin(); mi != queuedChanges.end(); ++mi) {
        if (!UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
    }
    
    if (block.getTransaction(0).GetValueOut() > _chain.subsidy(pindex->nHeight) + fees)
        return false;
    
    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev) {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }
    
    // Watch for transactions paying to me
    //    BOOST_FOREACH(Transaction& tx, vtx)
    //        SyncWithWallets(tx, this, true);
    
    return true;
}

bool BlockChain::reorganize(Block& block, CBlockIndex* pindexNew)
{
    printf("REORGANIZE\n");
    
    // Find the fork
    CBlockIndex* pfork = _bestIndex;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
        {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
        }
    
    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = _bestIndex; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);
    
    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());
    
    // Disconnect shorter branch
    vector<Transaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
    Block block;
    if (!_blockFile.readFromDisk(block, pindex))
        return error("Reorganize() : ReadFromDisk for disconnect failed");
    if (!disconnectBlock(block, pindex))
        return error("Reorganize() : DisconnectBlock failed");
    
    // Queue memory transactions to resurrect
    BOOST_FOREACH(const Transaction& tx, block.getTransactions())
    if (!tx.IsCoinBase())
        vResurrect.push_back(tx);
    }
    
    // Connect longer branch
    vector<Transaction> vDelete;
    for (int i = 0; i < vConnect.size(); i++) {
        CBlockIndex* pindex = vConnect[i];
        Block block;
        if (!_blockFile.readFromDisk(block,pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");
        if (!connectBlock(block, pindex)) {
            // Invalid block
            TxnAbort();
            return error("Reorganize() : ConnectBlock failed");
        }
        
        // Queue memory transactions to delete
        BOOST_FOREACH(const Transaction& tx, block.getTransactions())
        vDelete.push_back(tx);
    }
    _bestChain = pindexNew->GetBlockHash();
    if (!WriteHashBestChain())
        return error("Reorganize() : WriteHashBestChain failed");
    
    // Make sure it's successfully written to disk before changing memory structure
    if (!TxnCommit())
        return error("Reorganize() : TxnCommit failed");
    
    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    if (pindex->pprev)
        pindex->pprev->pnext = NULL;
    
    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
    if (pindex->pprev)
        pindex->pprev->pnext = pindex;
    
    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(Transaction& tx, vResurrect)
    AcceptToMemoryPool(tx, false);
    
    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(Transaction& tx, vDelete)
    RemoveFromMemoryPool(tx);
    
    return true;
}

void BlockChain::InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->bnChainWork > _bestInvalidWork) {
        _bestInvalidWork = pindexNew->bnChainWork;
        WriteBestInvalidWork();
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  work=%s\n", pindexNew->GetBlockHash().toString().substr(0,20).c_str(), pindexNew->nHeight, pindexNew->bnChainWork.toString().c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  work=%s\n", _bestChain.toString().substr(0,20).c_str(), getBestHeight(), _bestChainWork.toString().c_str());
    if (_bestIndex && _bestInvalidWork > _bestChainWork + _bestIndex->GetBlockWork() * 6)
        printf("InvalidChainFound: WARNING: Displayed transactions may not be correct!  You may need to upgrade, or other nodes may need to upgrade.\n");
}

bool BlockChain::setBestChain(Block& block, CBlockIndex* pindexNew)
{
    uint256 hash = block.getHash();
    
    uint256 oldBestChain = _bestChain;
    TxnBegin();
    if (_genesisBlockIndex == NULL && hash == getGenesisHash()) {
        _bestChain = hash;
        WriteHashBestChain();
        _bestChain = oldBestChain;
        if (!TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
        _genesisBlockIndex = pindexNew;
    }
    else if (block.getPrevBlock() == _bestChain) {
        // Adding to current best branch
        _bestChain = hash;
        if (!connectBlock(block, pindexNew) || !WriteHashBestChain()) {
            _bestChain = oldBestChain;
            TxnAbort();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : ConnectBlock failed");
        }
        if (!TxnCommit()) {
            _bestChain = oldBestChain;
            return error("SetBestChain() : TxnCommit failed");
        }
        
        // Add to current best branch
        pindexNew->pprev->pnext = pindexNew;
        
        // Delete redundant memory transactions
        for(int i = 0; i < block.getNumTransactions(); ++i)
            RemoveFromMemoryPool(block.getTransaction(i));
    }
    else {
        // New best branch
        if (!reorganize(block, pindexNew)) {
            TxnAbort();
            InvalidChainFound(pindexNew);
            _bestChain = oldBestChain;
            return error("SetBestChain() : Reorganize failed");
        }
    }
    /*
     // Update best block in wallet (so we can detect restored wallets)
     if (!isInitialBlockDownload())
     {
     const CBlockLocator locator(pindexNew);
     ::SetBestChain(locator);
     }
     */
    // New best block
    _bestChain = hash;
    _bestIndex = pindexNew;
    _bestChainWork = pindexNew->bnChainWork;
    _bestReceivedTime = GetTime();
    _transactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  work=%s\n", _bestChain.toString().substr(0,20).c_str(), getBestHeight(), _bestChainWork.toString().c_str());
    
    return true;
}


bool BlockChain::addToBlockIndex(Block& block, unsigned int nFile, unsigned int nBlockPos)
{
    // Check for duplicate
    uint256 hash = block.getHash();
    if (_blockChainIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.toString().substr(0,20).c_str());
    
    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, block);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    BlockChainIndex::iterator mi = _blockChainIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockChainIndex::iterator miPrev = _blockChainIndex.find(block.getPrevBlock());
    if (miPrev != _blockChainIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->bnChainWork = (pindexNew->pprev ? pindexNew->pprev->bnChainWork : 0) + pindexNew->GetBlockWork();
    
    TxnBegin();
    WriteBlockIndex(CDiskBlockIndex(pindexNew));
    if (!TxnCommit())
        return false;
    
    // New best
    if (pindexNew->bnChainWork > _bestChainWork)
        if (!setBestChain(block, pindexNew))
            return false;
    
    if (pindexNew == _bestIndex) {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        //        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = block.getTransaction(0).GetHash();
    }
    
    return true;
}

bool BlockChain::acceptBlock(Block& block)
{
    // Check for duplicate
    uint256 hash = block.getHash();
    if (_blockChainIndex.count(hash))
        return error("AcceptBlock() : block already in _blockChainIndex");
    
    // Get prev block index
    BlockChainIndex::iterator mi = _blockChainIndex.find(block.getPrevBlock());
    if (mi == _blockChainIndex.end())
        return error("AcceptBlock() : prev block not found");
    CBlockIndex* pindexPrev = (*mi).second;
    int height = pindexPrev->nHeight+1;
    
    // Check proof of work
    if (block.getBits() != _chain.nextWorkRequired(pindexPrev))
        return error("AcceptBlock() : incorrect proof of work");
    //   if (block.getBits() != GetNextWorkRequired(pindexPrev))
    //        return error("AcceptBlock() : incorrect proof of work");
    
    // Check timestamp against prev
    if (block.getBlockTime() <= pindexPrev->GetMedianTimePast())
        return error("AcceptBlock() : block's timestamp is too early");
    
    // Check that all transactions are finalized
    for(int i = 0; i < block.getNumTransactions(); ++i)
        if(!isFinal(block.getTransaction(i), height, block.getBlockTime()))
        return error("AcceptBlock() : contains a non-final transaction");
    
    // Check that the block chain matches the known block chain up to a checkpoint
    if(!_chain.checkPoints(height, hash))
        return error("AcceptBlock() : rejected by checkpoint lockin at %d", height);

    // Write block to history file
    if (!_blockFile.checkDiskSpace(::GetSerializeSize(block, SER_DISK)))
        return error("AcceptBlock() : out of disk space");
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    bool commit = (!isInitialBlockDownload() || (getBestHeight()+1) % 500 == 0);
    if (!_blockFile.writeToDisk(_chain, block, nFile, nBlockPos, commit))
        return error("AcceptBlock() : WriteToDisk failed");
    if (!addToBlockIndex(block, nFile, nBlockPos))
        return error("AcceptBlock() : AddToBlockIndex failed");
    
    return true;
}

bool BlockChain::CheckForMemoryPool(const Transaction& tx, Transaction*& ptxOld, bool fCheckInputs, bool* pfMissingInputs) const {
    if (pfMissingInputs)
        *pfMissingInputs = false;
    
    if (!tx.CheckTransaction())
        return error("AcceptToMemoryPool() : CheckTransaction failed");
    
    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return error("AcceptToMemoryPool() : coinbase as individual tx");
    
    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > INT_MAX)
        return error("AcceptToMemoryPool() : not accepting nLockTime beyond 2038 yet");
    
    // Safety limits
    unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK);
    // Checking ECDSA signatures is a CPU bottleneck, so to avoid denial-of-service
    // attacks disallow transactions with more than one SigOp per 34 bytes.
    // 34 bytes because a TxOut is:
    //   20-byte address + 8 byte bitcoin amount + 5 bytes of ops + 1 byte script length
    if (tx.GetSigOpCount() > nSize / 34 || nSize < 100)
        return error("AcceptToMemoryPool() : nonstandard transaction");
    
    // Rather not work on nonstandard transactions (unless -testnet)
    if (!_chain.isStandard(tx))
        return error("AcceptToMemoryPool() : nonstandard transaction type");
    
    // Do we already have it?
    uint256 hash = tx.GetHash();
    if (_transactionIndex.count(hash))
        return false;
    if (fCheckInputs)
        if (haveTx(hash, true))
            return false;
    
    // Check for conflicts with in-memory transactions
    //    Transaction* ptxOld = NULL;
    for (int i = 0; i < tx.vin.size(); i++) {
        Coin outpoint = tx.vin[i].prevout;
        TransactionConnections::const_iterator cnx = _transactionConnections.find(outpoint);
        if (cnx != _transactionConnections.end()) {
            // Disable replacement feature for now
            return false;
            
            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            ptxOld = (Transaction*)cnx->second.ptx;
            if (isFinal(*ptxOld))
                return false;
            if (!tx.IsNewerThan(*ptxOld))
                return false;
            for (int i = 0; i < tx.vin.size(); i++) {
                Coin outpoint = tx.vin[i].prevout;
                TransactionConnections::const_iterator cnx = _transactionConnections.find(outpoint);
                if (cnx == _transactionConnections.end() || cnx->second.ptx != ptxOld)
                    return false;
            }
            break;
        }
    }
    
    if (fCheckInputs) {
        // Check against previous transactions
        map<uint256, TxIndex> mapUnused;
        int64 nFees = 0;
        if (!connectInputs(tx, mapUnused, DiskTxPos(1,1,1), getBestIndex(), nFees, false, false)) {
            if (pfMissingInputs)
                *pfMissingInputs = true;
            return error("AcceptToMemoryPool() : ConnectInputs failed %s", hash.toString().substr(0,10).c_str());
        }
        
        // Don't accept it if it can't get into a block
        if (nFees < tx.GetMinFee(1000, true, true))
            return error("AcceptToMemoryPool() : not enough fees");
        
        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make other's transactions take longer to confirm.
        if (nFees < MIN_RELAY_TX_FEE) {
            static CCriticalSection cs;
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();
            
            CRITICAL_BLOCK(cs) {
                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000 /*&& !IsFromMe(*this)*/)
                    return error("AcceptToMemoryPool() : free transaction rejected by rate limiter");
                if (fDebug)
                    printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }
    }
    
    return true;
}

bool BlockChain::AcceptToMemoryPool(const Transaction& tx, bool fCheckInputs, bool* pfMissingInputs) {
    Transaction* ptxOld = NULL;
    if(CheckForMemoryPool(tx, ptxOld, fCheckInputs, pfMissingInputs)) {
        // Store transaction in memory
        if (ptxOld) {
            printf("AcceptToMemoryPool() : replacing tx %s with new version\n", ptxOld->GetHash().toString().c_str());
            RemoveFromMemoryPool(*ptxOld);
        }
        AddToMemoryPoolUnchecked(tx);
        
        ///// are we sure this is ok when loading transactions or restoring block txes
        // If updated, erase old tx from wallet
        //    if (ptxOld)
        //        EraseFromWallets(ptxOld->GetHash());
        
        printf("AcceptToMemoryPool(): accepted %s\n", tx.GetHash().toString().substr(0,10).c_str());
        return true;
    }
    else
       return false;
}

bool BlockChain::AddToMemoryPoolUnchecked(const Transaction& tx)
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call AcceptToMemoryPool to properly check the transaction first.
    
    uint256 hash = tx.GetHash();
    
    typedef pair<uint160, unsigned int> AssetPair;
    vector<AssetPair> debits;
    vector<AssetPair> credits;
    
    // for each tx out in the newly added tx check for a pubkey or a pubkeyhash in the script
    for(unsigned int n = 0; n < tx.vout.size(); n++)
        debits.push_back(AssetPair(tx.vout[n].getAsset(), n));
    
    if(!tx.IsCoinBase()) {
        for(unsigned int n = 0; n < tx.vin.size(); n++) {
            const CTxIn& txin = tx.vin[n];
            Transaction prevtx;
            if(!readDiskTx(txin.prevout.hash, prevtx))
                continue; // OK ???
            CTxOut txout = prevtx.vout[txin.prevout.index];        
            
            credits.push_back(AssetPair(txout.getAsset(), n));
        }
    }
    
    for(vector<AssetPair>::iterator assetpair = debits.begin(); assetpair != debits.end(); ++assetpair)
        _debitIndex[assetpair->first].insert(Coin(hash, assetpair->second));
    
    for(vector<AssetPair>::iterator assetpair = credits.begin(); assetpair != credits.end(); ++assetpair)
        _creditIndex[assetpair->first].insert(Coin(hash, assetpair->second));
    
    _transactionIndex[hash] = tx;
    for (int i = 0; i < tx.vin.size(); i++)
        _transactionConnections[tx.vin[i].prevout] = CoinRef(&_transactionIndex[hash], i);
    _transactionsUpdated++;
    
    return true;
}


bool BlockChain::RemoveFromMemoryPool(Transaction& tx)
{
    // Remove transaction from memory pool
    
    uint256 hash = tx.GetHash();
    
    typedef pair<uint160, unsigned int> AssetPair;
    vector<AssetPair> debits;
    vector<AssetPair> credits;
    
    // for each tx out in the newly added tx check for a pubkey or a pubkeyhash in the script
    for(unsigned int n = 0; n < tx.vout.size(); n++)
        debits.push_back(AssetPair(tx.vout[n].getAsset(), n));
    
    if(!tx.IsCoinBase()) {
        for(unsigned int n = 0; n < tx.vin.size(); n++) {
            const CTxIn& txin = tx.vin[n];
            Transaction prevtx;
            if(!readDiskTx(txin.prevout.hash, prevtx))
                continue; // OK ???
            CTxOut txout = prevtx.vout[txin.prevout.index];        
            
            credits.push_back(AssetPair(txout.getAsset(), n));
        }
    }
    
    for(vector<AssetPair>::iterator assetpair = debits.begin(); assetpair != debits.end(); ++assetpair) {
        _debitIndex[assetpair->first].erase(Coin(hash, assetpair->second));
        if(_debitIndex[assetpair->first].size() == 0)
            _debitIndex.erase(assetpair->first); 
    }
    
    for(vector<AssetPair>::iterator assetpair = credits.begin(); assetpair != credits.end(); ++assetpair) {
        _creditIndex[assetpair->first].erase(Coin(hash, assetpair->second));
        if(_creditIndex[assetpair->first].size() == 0)
            _creditIndex.erase(assetpair->first); 
    }
    
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    _transactionConnections.erase(txin.prevout);
    _transactionIndex.erase(tx.GetHash());
    _transactionsUpdated++;
    
    return true;
}

void BlockChain::getCredit(const uint160& btc, Coins& coins) const
{
    readCrIndex(btc, coins);
    AssetIndex::const_iterator credit = _creditIndex.find(btc);
    if(credit != _creditIndex.end())
        coins.insert(credit->second.begin(), credit->second.end());
}

void BlockChain::getDebit(const uint160& btc, Coins& coins) const
{
    readDrIndex(btc, coins);
    AssetIndex::const_iterator debit = _debitIndex.find(btc);
    if(debit != _debitIndex.end())
        coins.insert(debit->second.begin(), debit->second.end());
}

void BlockChain::getTransaction(const uint256& hash, Transaction& tx) const
{
    tx.SetNull();
    if(!readDiskTx(hash, tx)) {
        TransactionIndex::const_iterator hashtx = _transactionIndex.find(hash);
        if(hashtx != _transactionIndex.end())
            tx = hashtx->second;
    }
}

void BlockChain::getTransaction(const uint256& hash, Transaction& tx, int64& height, int64& time) const
{
    tx.SetNull();
    height = -1;
    time = -1;
    if(!readDiskTx(hash, tx, height, time)) {
        TransactionIndex::const_iterator hashtx = _transactionIndex.find(hash);
        if(hashtx != _transactionIndex.end())
            tx = hashtx->second;
    }
}

bool BlockChain::isSpent(Coin coin) const {
    TxIndex index;
    if(ReadTxIndex(coin.hash, index))
        return !index.getSpent(coin.index).isNull();
    else
        return false;
}

int BlockChain::getNumSpent(uint256 hash) const {
    TxIndex index;
    if(ReadTxIndex(hash, index))
        return index.getNumSpents();
    else
        return 0;
}

uint256 BlockChain::spentIn(Coin coin) const {
    TxIndex index;
    if(ReadTxIndex(coin.hash, index)) {
        const DiskTxPos& diskpos = index.getSpent(coin.index);
        Transaction tx;
        _blockFile.readFromDisk(tx, diskpos);
        return tx.GetHash();
    }
    else
        return 0;    
}


//
// CDBAssetSyncronizer
//


void CDBAssetSyncronizer::getCreditCoins(uint160 btc, Coins& coins)
{
    _blockChain.getCredit(btc, coins);
}

void CDBAssetSyncronizer::getDebitCoins(uint160 btc, Coins& coins)
{
    _blockChain.getDebit(btc, coins);    
}

void CDBAssetSyncronizer::getTransaction(const Coin& coin, Transaction& tx)
{
    _blockChain.getTransaction(coin.hash, tx);
}

void CDBAssetSyncronizer::getCoins(uint160 btc, Coins& coins)
{
    // read all relevant tx'es
    Coins debit;
    getDebitCoins(btc, debit);
    Coins credit;
    getCreditCoins(btc, credit);
    
    for(Coins::iterator coin = debit.begin(); coin != debit.end(); ++coin) {
        Transaction tx;
        getTransaction(*coin, tx);
        coins.insert(*coin);
    }
    for(Coins::iterator coin = credit.begin(); coin != credit.end(); ++coin) {
        Transaction tx;
        getTransaction(*coin, tx);
        
        CTxIn in = tx.vin[coin->index];
        Coin spend(in.prevout.hash, in.prevout.index);
        coins.erase(spend);
    }
}
