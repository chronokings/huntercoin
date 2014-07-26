// Copyright (c) 2010-2011 Vincent Durham
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.
//
#include "headers.h"
#include "db.h"
#include "keystore.h"
#include "wallet.h"
#include "init.h"
#include "auxpow.h"
#include "huntercoin.h"

#include "gamestate.h"
#include "gamedb.h"
#include "gamemovecreator.h"
#include "gametx.h"

#include "bitcoinrpc.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace std;
using namespace json_spirit;

static const bool NAME_DEBUG = false;
extern int64 AmountFromValue(const Value& value);
extern Object JSONRPCError(int code, const string& message);
template<typename T> void ConvertTo(Value& value, bool fAllowNull=false);

/* Minimum mandatory fee for name_update transactions.  Transactions with
   a lower fee are valid but non-standard, to enforce protection against
   transaction spam in the blockchain.  If the transaction would require
   a larger fee due to the usual fee rules, then this is still true.  */
static const int64 NAME_UPDATE_MIN_FEE = COIN / 100;
/* Fee per (full) 100 characters name length for name_update.  This is in
   addition to the NAME_UPDATE_MIN_FEE.  If the ordinary fee due to
   transaction size is larger, the latter will be used instead.  */
static const int64 NAME_UPDATE_LEN_FEE = COIN / 500;

map<vector<unsigned char>, uint256> mapMyNames;
map<vector<unsigned char>, set<uint256> > mapNamePending;

boost::mutex mut_currentState;
boost::condition_variable cv_stateChange;

boost::mutex json_spirit::mtx_parser;


#ifdef GUI
extern std::map<uint160, std::vector<unsigned char> > mapMyNameHashes;
#endif

extern uint256 SignatureHash(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType);

// forward decls
extern bool Solver(const CKeyStore& keystore, const CScript& scriptPubKey, uint256 hash, int nHashType, CScript& scriptSigRet);
extern bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CTransaction& txTo, unsigned int nIn, int nHashType);
extern void rescanfornames();
extern Value sendtoaddress(const Array& params, bool fHelp);

// The following value is assigned to the name when the player is dead.
// It must not be a valid move JSON string, because it is checked in NameAvailable
// as a shortcut to reading tx and checking IsGameTx.
const static std::string VALUE_DEAD("{\"dead\":1}");

uint256 hashHuntercoinGenesisBlock[2] = {
        uint256("00000000db7eb7a9e1a06cf995363dcdc4c28e8ae04827a961942657db9a1631"),    // Main net
        uint256("000000492c361a01ce7558a3bfb198ea3ff2f86f8b0c2e00d26135c53f4acbf7")     // Test net
    };

class CHuntercoinHooks : public CHooks
{
public:
    virtual bool IsStandard(const CScript& scriptPubKey);
    virtual void AddToWallet(CWalletTx& tx);
    virtual bool CheckTransaction(const CTransaction& tx);
    virtual bool ConnectInputs(DatabaseSet& dbset,
            const std::map<uint256, CTxIndex>& mapTestPool,
            const CTransaction& tx,
            const std::vector<CUtxoEntry>& vTxoPrev,
            const CBlockIndex* pindexBlock,
            const CDiskTxPos& txPos,
            bool fBlock,
            bool fMiner);
    virtual bool DisconnectInputs (DatabaseSet& dbset,
            const CTransaction& tx,
            CBlockIndex* pindexBlock);
    virtual bool ConnectBlock (CBlock& block, DatabaseSet& txdb,
                               CBlockIndex* pindex, int64 &nFees,
                               unsigned int nPosAfterTx);
    virtual void NewBlockAdded ();
    virtual bool DisconnectBlock (CBlock& block, DatabaseSet& txdb,
                                  CBlockIndex* pindex);
    virtual bool ExtractAddress(const CScript& script, string& address);
    virtual bool GenesisBlock(CBlock& block);
    virtual bool Lockin(int nHeight, uint256 hash);
    virtual int LockinHeight();
    virtual string IrcPrefix();
    virtual void AcceptToMemoryPool (DatabaseSet& dbset,
                                     const CTransaction& tx);
    virtual void RemoveFromMemoryPool (const CTransaction& tx);

    virtual void MessageStart(char* pchMessageStart)
    {
        // Make the message start different
        pchMessageStart[3] = 0xfe;
    }
    virtual bool IsMine(const CTransaction& tx);
    virtual bool IsMine(const CTransaction& tx, const CTxOut& txout, bool ignore_name_new = false);

    virtual void GetMinFee(int64 &nMinFee, int64 &nBaseFee, const CTransaction &tx,
        unsigned int nBlockSize, bool fAllowFree, bool fForRelay,
        unsigned int nBytes, unsigned int nNewBlockSize);

    virtual bool CheckFees (const CTransaction& tx, int64 nFees);

    string GetAlertPubkey1()
    {
        // Huntercoin alert pubkey
        return "04d55568f5688898159fd01640f6c7ef2e63fef95376e8418244b4c7c4dd57110d8028f4086a092f2586dc09b36359e67e0717a0bec2a483c81aaf252377fc666a";
    }

private:

    /* Utility routine to check if a tx is a name_update.  This is used to
       enforce the mandatory fee both in the GetMinFee and IsStandard hooks.
       If true, it also returns the value the name is updated to.  */
    static bool IsNameUpdate (const CTransaction& tx, vchType& value);

    /* Calculate fee for name_update operations based on the value length.  */
    inline static int64
    NameUpdateFee (const vchType& value)
    {
      return NAME_UPDATE_MIN_FEE + NAME_UPDATE_LEN_FEE * (value.size () / 100);
    }

};

bool
CHuntercoinHooks::IsNameUpdate (const CTransaction& tx, vchType& value)
{
  if (tx.nVersion != NAMECOIN_TX_VERSION)
    return false;

  vector<vchType> vvch;
  int op;
  int nOut;
  if (!DecodeNameTx(tx, op, nOut, vvch))
    return false;

  if (op != OP_NAME_UPDATE)
    return false;

  value = vvch[1];
  return true;
}

void
CHuntercoinHooks::GetMinFee(int64 &nMinFee, int64 &nBaseFee,
    const CTransaction &tx,
    unsigned int nBlockSize, bool fAllowFree, bool fForRelay,
    unsigned int nBytes, unsigned int nNewBlockSize)
{
    vchType value;
    if (!IsNameUpdate (tx, value))
        return;

    /* Enforce minimum mandatory fee for moves.  */
    const int64 updateFee = NameUpdateFee (value);
    if (nMinFee < updateFee)
        nMinFee = updateFee;
}

bool
CHuntercoinHooks::CheckFees (const CTransaction& tx, int64 nFees)
{
  vchType value;
  if (!IsNameUpdate (tx, value))
    return true;

  return (nFees >= NameUpdateFee (value));
}

bool
CHuntercoinHooks::IsStandard(const CScript& scriptPubKey)
{
    return true;
}

int64 getAmount(Value value)
{
    ConvertTo<double>(value);
    double dAmount = value.get_real();
    int64 nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    return nAmount;
}

vchType
vchFromValue (const Value& value)
{
  const std::string str = value.get_str ();
  return vchFromString (str);
}

vchType
vchFromString (const std::string& str)
{
  const unsigned char* strbeg;
  strbeg = reinterpret_cast<const unsigned char*> (str.c_str ());
  return vchType(strbeg, strbeg + str.size ());
}

string stringFromVch(const vector<unsigned char> &vch) {
    string res;
    vector<unsigned char>::const_iterator vi = vch.begin();
    while (vi != vch.end()) {
        res += (char)(*vi);
        vi++;
    }
    return res;
}

int64 GetNetworkFee(int nHeight)
{
        return 0;
}

/* Return the minimum necessary amount of locked coins.  This replaces the
   old NAME_COIN_AMOUNT constant and makes it more dynamic, so that we can
   change it with hard forks.  If the frontEnd flag is set, return the
   amount advised for the front-ends and not the protocol-enforced one.
   This allows to increase the amount for front-ends earlier than when
   it is enforced in the protocol, so that a prepared name_firstupdate
   won't get stuck.  */
int64
GetNameCoinAmount (unsigned nHeight, bool frontEnd)
{
  unsigned forkHeight = FORK_HEIGHT_POISON;

  /* For front-ends, increase the amount a little earlier.  */
  if (frontEnd)
    forkHeight -= 10;

  return (nHeight < forkHeight ? COIN : 10 * COIN);
}

bool
IsValidPlayerName (const std::string& player)
{
    if (player.size () > MAX_NAME_LENGTH)
      return false;

    // Check player name validity
    // Can contain letters, digits, underscore, hyphen and whitespace
    // Cannot contain double whitespaces or start/end with whitespace
    using namespace boost::xpressive;
    static sregex regex = sregex::compile("^([a-zA-Z0-9_-]+ )*[a-zA-Z0-9_-]+$");
    smatch match;
    return regex_search(player, match, regex);
}

int GetTxPosHeight(const CNameIndex& txPos)
{
    return txPos.nHeight;
}

int GetTxPosHeight(const CDiskTxPos& txPos)
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(txPos.nBlockFile, txPos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    const CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return pindex->nHeight;
}

bool
NameAvailable (DatabaseSet& dbset, const vector<unsigned char> &vchName)
{
    vector<CNameIndex> vtxPos;
    if (!dbset.name ().ExistsName (vchName))
        return true;

    if (!dbset.name ().ReadName (vchName, vtxPos))
        return error("NameAvailable() : failed to read from name DB");
    if (vtxPos.empty())
        return true;

    CNameIndex& txPos = vtxPos.back();

    // If player is dead, a new player with the same name can be created
    if (txPos.vValue == vchFromString(VALUE_DEAD))
        return true;

    return false;
}

CScript RemoveNameScriptPrefix(const CScript& scriptIn)
{
    int op;
    vector<vector<unsigned char> > vvch;
    CScript::const_iterator pc = scriptIn.begin();

    if (!DecodeNameScript(scriptIn, op, vvch,  pc))
        throw runtime_error("RemoveNameScriptPrefix() : could not decode name script");
    return CScript(pc, scriptIn.end());
}

static bool
IsMyName (const CTxOut& txout)
{
  const CScript scriptPubKey = RemoveNameScriptPrefix (txout.scriptPubKey);
  CScript scriptSig;

  return Solver (*pwalletMain, scriptPubKey, 0, 0, scriptSig);
}

bool CreateTransactionWithInputTx(const vector<pair<CScript, int64> >& vecSend, const CWalletTx& wtxIn, int nTxOut, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet)
{
    int64 nValue = 0;
    BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
    {
        if (nValue < 0)
            return false;
        nValue += s.second;
    }
    if (vecSend.empty() || nValue < 0)
        return false;

    wtxNew.pwallet = pwalletMain;

    CRITICAL_BLOCK(cs_main)
    {
        // txdb must be opened before the mapWallet lock
        CTxDB txdb("r");
        CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
        {
            nFeeRet = nTransactionFee;
            loop
            {
                wtxNew.vin.clear();
                wtxNew.vout.clear();
                wtxNew.fFromMe = true;

                int64 nTotalValue = nValue + nFeeRet;
                printf("CreateTransactionWithInputTx: total value = %d\n", nTotalValue);
                double dPriority = 0;
                // vouts to the payees
                BOOST_FOREACH(const PAIRTYPE(CScript, int64)& s, vecSend)
                    wtxNew.vout.push_back(CTxOut(s.second, s.first));

                int64 nWtxinCredit = wtxIn.vout[nTxOut].nValue;

                // Choose coins to use
                set<pair<const CWalletTx*, unsigned int> > setCoins;
                int64 nValueIn = 0;
                printf("CreateTransactionWithInputTx: SelectCoins(%s), nTotalValue = %s, nWtxinCredit = %s\n", FormatMoney(nTotalValue - nWtxinCredit).c_str(), FormatMoney(nTotalValue).c_str(), FormatMoney(nWtxinCredit).c_str());
                if (nTotalValue - nWtxinCredit > 0)
                {
                    if (!pwalletMain->SelectCoins(nTotalValue - nWtxinCredit, setCoins, nValueIn))
                        return false;
                }

                printf("CreateTransactionWithInputTx: selected %d tx outs, nValueIn = %s\n", setCoins.size(), FormatMoney(nValueIn).c_str());

                vector<pair<const CWalletTx*, unsigned int> >
                    vecCoins(setCoins.begin(), setCoins.end());

                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    int64 nCredit = coin.first->vout[coin.second].nValue;
                    dPriority += (double)nCredit * coin.first->GetDepthInMainChain();
                }

                // Input tx always at first position
                vecCoins.insert(vecCoins.begin(), make_pair(&wtxIn, nTxOut));

                nValueIn += nWtxinCredit;
                dPriority += (double)nWtxinCredit * wtxIn.GetDepthInMainChain();

                // Fill a vout back to self with any change
                int64 nChange = nValueIn - nTotalValue;
                if (nChange >= CENT)
                {
                    // Note: We use a new key here to keep it from being obvious which side is the change.
                    //  The drawback is that by not reusing a previous key, the change may be lost if a
                    //  backup is restored, if the backup doesn't have the new private key for the change.
                    //  If we reused the old key, it would be possible to add code to look for and
                    //  rediscover unknown transactions that were written with keys of ours to recover
                    //  post-backup change.

                    // Reserve a new key pair from key pool
                    vector<unsigned char> vchPubKey = reservekey.GetReservedKey();
                    assert(pwalletMain->HaveKey(vchPubKey));

                    CScript scriptChange;
                    if (vecSend[0].first.GetBitcoinAddressHash160() != 0)
                        scriptChange.SetBitcoinAddress(vchPubKey);
                    else
                        scriptChange << vchPubKey << OP_CHECKSIG;

                    // Insert change txn at random position:
                    vector<CTxOut>::iterator position = wtxNew.vout.begin()+GetRandInt(wtxNew.vout.size());
                    wtxNew.vout.insert(position, CTxOut(nChange, scriptChange));
                }
                else
                    reservekey.ReturnKey();

                // Fill vin
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                    wtxNew.vin.push_back(CTxIn(coin.first->GetHash(), coin.second));

                // Sign
                int nIn = 0;
                BOOST_FOREACH(PAIRTYPE(const CWalletTx*, unsigned int)& coin, vecCoins)
                {
                    if (!SignSignature(*pwalletMain, *coin.first, wtxNew, nIn++))
                        return false;
                }

                // Limit size
                unsigned int nBytes = ::GetSerializeSize(*(CTransaction*)&wtxNew, SER_NETWORK);
                if (nBytes >= MAX_BLOCK_SIZE_GEN/5)
                    return false;
                dPriority /= nBytes;

                // Check that enough fee is included
                int64 nPayFee = nTransactionFee * (1 + (int64)nBytes / 1000);
                bool fAllowFree = CTransaction::AllowFree(dPriority);
                int64 nMinFee = wtxNew.GetMinFee(1, fAllowFree);
                if (nFeeRet < max(nPayFee, nMinFee))
                {
                    nFeeRet = max(nPayFee, nMinFee);
                    printf("CreateTransactionWithInputTx: re-iterating (nFreeRet = %s)\n", FormatMoney(nFeeRet).c_str());
                    continue;
                }

                // Fill vtxPrev by copying from previous transactions vtxPrev
                wtxNew.AddSupportingTransactions(txdb);
                wtxNew.fTimeReceivedIsTxTime = true;

                break;
            }
        }
    }
    printf("CreateTransactionWithInputTx succeeded:\n%s", wtxNew.ToString().c_str()); 
    return true;
}

// nTxOut is the output from wtxIn that we should grab
// requires cs_main lock
string SendMoneyWithInputTx(const CScript& scriptPubKey, int64 nValue, int64 nNetFee, const CWalletTx& wtxIn, CWalletTx& wtxNew, bool fAskFee)
{
    if (wtxIn.IsGameTx())
        return _("Error: SendMoneyWithInputTx trying to spend a game-created transaction");
    int nTxOut = IndexOfNameOutput(wtxIn);
    CReserveKey reservekey(pwalletMain);
    int64 nFeeRequired;
    vector< pair<CScript, int64> > vecSend;
    vecSend.push_back(make_pair(scriptPubKey, nValue));

    if (nNetFee)
    {
        CScript scriptFee;
        scriptFee << OP_RETURN;
        vecSend.push_back(make_pair(scriptFee, nNetFee));
    }

    if (!CreateTransactionWithInputTx(vecSend, wtxIn, nTxOut, wtxNew, reservekey, nFeeRequired))
    {
        string strError;
        if (nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf(_("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds "), FormatMoney(nFeeRequired).c_str());
        else
            strError = _("Error: Transaction creation failed  ");
        printf("SendMoneyWithInputTx() : %s", strError.c_str());
        return strError;
    }

#ifdef GUI
    if (fAskFee && !uiInterface.ThreadSafeAskFee(nFeeRequired))
        return "ABORTED";
#else
    if (fAskFee && !ThreadSafeAskFee(nFeeRequired, "Huntercoin", NULL))
        return "ABORTED";
#endif

    if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
        return _("Error: The transaction was rejected.  This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");

    return "";
}

bool GetValueOfTxPos(const CNameIndex& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    vchValue = txPos.vValue;
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    hash = tx.GetHash();
    return true;
}

bool GetValueOfTxPos(const CDiskTxPos& txPos, vector<unsigned char>& vchValue, uint256& hash, int& nHeight)
{
    nHeight = GetTxPosHeight(txPos);
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos))
        return error("GetValueOfTxPos() : could not read tx from disk");
    if (!GetValueOfNameTx(tx, vchValue))
        return error("GetValueOfTxPos() : could not decode value from tx");
    hash = tx.GetHash();
    return true;
}

bool GetValueOfName(CNameDB& dbName, const vector<unsigned char> &vchName, vector<unsigned char>& vchValue, int& nHeight)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;

    CNameIndex& txPos = vtxPos.back();
    nHeight = txPos.nHeight;
    vchValue = txPos.vValue;
    return true;
}

bool GetTxOfName(CNameDB& dbName, const vector<unsigned char> &vchName, CTransaction& tx)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    CNameIndex& txPos = vtxPos.back();
    if (!tx.ReadFromDisk(txPos.txPos))
        return error("GetTxOfName() : could not read tx from disk");
    return true;
}

// Returns only player move transactions, i.e. ignores game-created transaction (player death)
bool GetTxOfNameAtHeight(CNameDB& dbName, const std::vector<unsigned char> &vchName, int nHeight, CTransaction& tx)
{
    vector<CNameIndex> vtxPos;
    if (!dbName.ReadName(vchName, vtxPos) || vtxPos.empty())
        return false;
    int i = vtxPos.size();

    loop
    {
        // Find maximum height less or equal to nHeight
        // TODO: binary search (preferably with bias towards the last element, e.g. split at 3/4)
        do
        {
            if (i == 0)
                return false;
            i--;
        } while (vtxPos[i].nHeight > nHeight);
        if (!tx.ReadFromDisk(vtxPos[i].txPos))
            return error("GetTxOfNameAtHeight() : could not read tx from disk");

        if (!tx.IsGameTx())
            return true;

        // If game transaction found (player death) proceed to the previous transaction
    }
}

bool GetNameAddress(const CTransaction& tx, std::string& strAddress)
{
    uint160 hash160;
    if (!GetNameAddress(tx, hash160))
        return false;
    strAddress = Hash160ToAddress(hash160);
    return true;
}

bool GetNameAddress(const CTransaction& tx, uint160 &hash160)
{
    int op;
    int nOut;
    vector<vector<unsigned char> > vvch;
    DecodeNameTx(tx, op, nOut, vvch);
    const CTxOut& txout = tx.vout[nOut];
    const CScript& scriptPubKey = RemoveNameScriptPrefix(txout.scriptPubKey);
    hash160 = scriptPubKey.GetBitcoinAddressHash160();
    return true;
}

bool GetNameAddress(const CDiskTxPos& txPos, std::string& strAddress)
{
    CTransaction tx;
    if (!tx.ReadFromDisk(txPos))
        return error("GetNameAddress() : could not read tx from disk");

    return GetNameAddress(tx, strAddress);
}

Value sendtoname(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendtoname <name> <amount> [comment] [comment-to]\n"
            "<amount> is a real and is rounded to the nearest 0.01"
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    CNameDB dbName("r");
    if (!dbName.ExistsName(vchName))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Name not found");

    string strAddress;
    CTransaction tx;
    GetTxOfName(dbName, vchName, tx);
    GetNameAddress(tx, strAddress);

    uint160 hash160;
    if (!AddressToHash160(strAddress, hash160))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No valid huntercoin address");

    // Amount
    int64 nAmount = AmountFromValue(params[1]);

    // Wallet comments
    CWalletTx wtx;
    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["to"]      = params[3].get_str();

    CRITICAL_BLOCK(cs_main)
    {  
        EnsureWalletIsUnlocked();

        string strError = pwalletMain->SendMoneyToBitcoinAddress(strAddress, nAmount, wtx);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    return wtx.GetHash().GetHex();
}

Value name_list(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
                "name_list [<name>]\n"
                "list my own names"
                );

    vchType vchNameUniq;
    if (params.size () == 1)
      vchNameUniq = vchFromValue (params[0]);

    Array oRes;
    std::map<vchType, int> vNamesI;
    std::map<vchType, Object> vNamesO;

    /* For determining the tx height by the txindex, one has to load the
       block header from disk.  To prevent this, we look up the name index
       and use that instead.  Cache the name index lookups for better
       performance.  */
    std::map<vchType, std::vector<CNameIndex> > nameIndexCache;

    /* Collect some info for performance optimisation.  We store the total
       number of transactions processed (which were name tx) and the
       number that needed to be loaded from disk (its txindex) since
       they couldn't be short-cut.  */
    unsigned totalTx = 0;
    unsigned loadedTx = 0;

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
      {
        CTxDB txdb("r");
        CNameDB namedb("r");

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item,
                      pwalletMain->mapWallet)
          {
            const CWalletTx& tx = item.second;

            vchType vchName, vchValue;
            int nOut;
            if (!tx.GetNameUpdate (nOut, vchName, vchValue))
              continue;

            ++totalTx;

            if (!vchNameUniq.empty () && vchNameUniq != vchName)
              continue;

            /* The expensive part of this routine is loading the
               tx index from disk later on.  To improve the situation
               especially for wallets with loads of name_update operations
               (typical for Huntercoin), we bail out early if the name
               output of the transaction is already spent (since then,
               a follow-up transaction will occur later anyway).

               Note that the vfSpent array we check here (for a wallet
               transaction) only tracks outpoints owned by the wallet user.
               Thus it is also no problem if someone else spends the output
               after transferring a name to them, even though in that case
               *no* follow-up wtx will appear in the loop.

               The only thing to watch out for is if we just did a name_update
               and have a pending transaction in the wallet.  In that case,
               the output will already be marked as spent in the wallet, but
               we won't get a later entry (since it fails the txindex lookup
               later on as an unconfirmed transaction).  Thus never apply
               this shortcut to names which appear in mapNamePending.  */

            if (nOut < tx.vfSpent.size ()
                && mapNamePending.count (vchName) == 0
                && tx.IsSpent (nOut))
              continue;
            
            ++loadedTx;

            /* Load tx index for disk position and spent-type array.  */
            CTxIndex txindex;
            if (!txdb.ReadTxIndex (tx.GetHash (), txindex))
              continue;

            /* Find the name's name index object to get the height.  */
            if (nameIndexCache.count (vchName) == 0)
              {
                std::vector<CNameIndex> data;
                if (!namedb.ReadName (vchName, data))
                  {
                    error ("name_list: ReadName failed");
                    continue;
                  }
                nameIndexCache[vchName] = data;
              }
            const std::vector<CNameIndex> vNmIndex = nameIndexCache[vchName];
            int nHeight = -1;
            for (std::vector<CNameIndex>::const_iterator i = vNmIndex.begin ();
                 i != vNmIndex.end (); ++i)
              if (i->txPos == txindex.pos)
                {
                  nHeight = i->nHeight;
                  break;
                }
            if (nHeight == -1)
              {
                error ("name_list: txpos not found in name index");
                continue;
              }

            // get last active name only
            if (vNamesI.find (vchName) != vNamesI.end ()
                && vNamesI[vchName] > nHeight)
              continue;

            Object oName;
            const std::string sName = stringFromVch(vchName);
            oName.push_back(Pair("name", sName));
            oName.push_back(Pair("value", stringFromVch(vchValue)));
            if (!hooks->IsMine (tx))
                oName.push_back(Pair("transferred", 1));
            std::string strAddress;
            GetNameAddress(tx, strAddress);
            oName.push_back(Pair("address", strAddress));

            if (IsPlayerDead (tx, txindex))
              oName.push_back (Pair("dead", 1));

            vNamesI[vchName] = nHeight;
            vNamesO[vchName] = oName;
          }
      }

    BOOST_FOREACH(const PAIRTYPE(vchType, Object)& item, vNamesO)
      oRes.push_back(item.second);

    printf ("name_list: total %u name tx, loaded %u.\n", totalTx, loadedTx);

    return oRes;
}

Value name_debug(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 0)
        throw runtime_error(
            "name_debug\n"
            "Dump pending transactions id in the debug file.\n");

    printf("Pending:\n----------------------------\n");
    pair<vector<unsigned char>, set<uint256> > pairPending;

    CRITICAL_BLOCK(cs_main)
        BOOST_FOREACH(pairPending, mapNamePending)
        {
            string name = stringFromVch(pairPending.first);
            printf("%s :\n", name.c_str());
            uint256 hash;
            BOOST_FOREACH(hash, pairPending.second)
            {
                printf("    ");
                if (!pwalletMain->mapWallet.count(hash))
                    printf("foreign ");
                printf("    %s\n", hash.GetHex().c_str());
            }
        }
    printf("----------------------------\n");
    return true;
}

Value name_debug1(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1)
        throw runtime_error(
            "name_debug1 <name>\n"
            "Dump name blocks number and transactions id in the debug file.\n");

    vector<unsigned char> vchName = vchFromValue(params[0]);
    printf("Dump name:\n");
    CRITICAL_BLOCK(cs_main)
    {
        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        CNameDB dbName("r");
        if (!dbName.ReadName(vchName, vtxPos))
        {
            error("failed to read from name DB");
            return false;
        }
        //CDiskTxPos txPos;
        CNameIndex txPos;
        BOOST_FOREACH(txPos, vtxPos)
        {
            CTransaction tx;
            if (!tx.ReadFromDisk(txPos.txPos))
            {
                error("could not read txpos %s", txPos.txPos.ToString().c_str());
                continue;
            }
            printf("@%d %s\n", GetTxPosHeight(txPos), tx.GetHash().GetHex().c_str());
        }
    }
    printf("-------------------------\n");
    return true;
}

Value name_show(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "name_show <name>\n"
            "Show values of a name.\n"
            );

    Object oLastName;
    vector<unsigned char> vchName = vchFromValue(params[0]);
    string name = stringFromVch(vchName);
    CRITICAL_BLOCK(cs_main)
    {
        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        CNameDB dbName("r");
        if (!dbName.ReadName(vchName, vtxPos))
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

        if (vtxPos.size() < 1)
            throw JSONRPCError(RPC_WALLET_ERROR, "no result returned");

        CDiskTxPos txPos = vtxPos.back().txPos;
        CTransaction tx;
        if (!tx.ReadFromDisk(txPos))
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from from disk");

        Object oName;
        vector<unsigned char> vchValue;
        int nHeight;
        uint256 hash;

        if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
        {
            oName.push_back(Pair("name", name));
            string value = stringFromVch(vchValue);
            oName.push_back(Pair("value", value));
            oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            string strAddress = "";
            GetNameAddress(txPos, strAddress);
            oName.push_back(Pair("address", strAddress));
            oLastName = oName;
        }
        else if (tx.IsGameTx ())
        {
            oName.push_back(Pair("name", name));
            oName.push_back(Pair("value", VALUE_DEAD));
            oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            oName.push_back(Pair("dead", 1));
            oLastName = oName;
        }
    }
    return oLastName;
}

Value name_history(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "name_history <name>\n"
            "List all name values of a name.\n");

    Array oRes;
    vector<unsigned char> vchName = vchFromValue(params[0]);
    string name = stringFromVch(vchName);
    CRITICAL_BLOCK(cs_main)
    {
        //vector<CDiskTxPos> vtxPos;
        vector<CNameIndex> vtxPos;
        CNameDB dbName("r");
        if (!dbName.ReadName(vchName, vtxPos))
            throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from name DB");

        CNameIndex txPos2;
        CDiskTxPos txPos;
        BOOST_FOREACH(txPos2, vtxPos)
        {
            txPos = txPos2.txPos;
            CTransaction tx;
            if (!tx.ReadFromDisk(txPos))
            {
                error("could not read txpos %s", txPos.ToString().c_str());
                continue;
            }

            Object oName;
            vector<unsigned char> vchValue;
            int nHeight;
            uint256 hash;

            if (!txPos.IsNull() && GetValueOfTxPos(txPos, vchValue, hash, nHeight))
            {
                oName.push_back(Pair("name", name));
                string value = stringFromVch(vchValue);
                oName.push_back(Pair("value", value));
                oName.push_back(Pair("txid", tx.GetHash().GetHex()));
                string strAddress = "";
                GetNameAddress(txPos, strAddress);
                oName.push_back(Pair("address", strAddress));
                oRes.push_back(oName);
            }
            else if (tx.IsGameTx ())
            {
                oName.push_back(Pair("name", name));
                oName.push_back(Pair("value", VALUE_DEAD));
                oName.push_back(Pair("txid", tx.GetHash().GetHex()));
                oName.push_back(Pair("dead", 1));
                oRes.push_back(oName);
            }
        }
    }
    return oRes;
}

Value name_filter(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
                "name_filter [regexp] [maxage=36000] [from=0] [nb=0] [stat]\n"
                "scan and filter names\n"
                );

    string strRegexp;
    int nFrom = 0;
    int nNb = 0;
    int nMaxAge = 36000;
    bool fStat = false;
    int nCountFrom = 0;
    int nCountNb = 0;


    if (params.size() > 0)
        strRegexp = params[0].get_str();

    if (params.size() > 1)
        nMaxAge = params[1].get_int();

    if (params.size() > 2)
        nFrom = params[2].get_int();

    if (params.size() > 3)
        nNb = params[3].get_int();

    if (params.size() > 4)
        fStat = (params[4].get_str() == "stat" ? true : false);


    CNameDB dbName("r");
    Array oRes;

    vector<unsigned char> vchName;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, 100000000, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        string name = stringFromVch(pairScan.first);

        // regexp
        using namespace boost::xpressive;
        smatch nameparts;
        sregex cregex = sregex::compile(strRegexp);
        if(strRegexp != "" && !regex_search(name, nameparts, cregex))
            continue;

        CNameIndex txName = pairScan.second;
        int nHeight = txName.nHeight;

        // max age
        if(nMaxAge != 0 && pindexBest->nHeight - nHeight >= nMaxAge)
            continue;

        // from limits
        nCountFrom++;
        if(nCountFrom < nFrom + 1)
            continue;

        Object oName;
        oName.push_back(Pair("name", name));
        CTransaction tx;
        CDiskTxPos txPos = txName.txPos;
        if (!txPos.IsNull() && tx.ReadFromDisk(txPos))
        {
            vector<unsigned char> vchValue = txName.vValue;
            string value = stringFromVch(vchValue);
            oName.push_back(Pair("value", value));
        }
        oRes.push_back(oName);

        nCountNb++;
        // nb limits
        if(nNb > 0 && nCountNb >= nNb)
            break;
    }

    if (NAME_DEBUG) {
        dbName.test();
    }

    if(fStat)
    {
        Object oStat;
        oStat.push_back(Pair("blocks",    (int)nBestHeight));
        oStat.push_back(Pair("count",     (int)oRes.size()));
        //oStat.push_back(Pair("sha256sum", SHA256(oRes), true));
        return oStat;
    }

    return oRes;
}

Value name_scan(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
                "name_scan [<start-name>] [<max-returned>]\n"
                "scan all names, starting at start-name and returning a maximum number of entries (default 500)\n"
                );

    vector<unsigned char> vchName;
    int nMax = 500;
    if (params.size() > 0)
    {
        vchName = vchFromValue(params[0]);
    }

    if (params.size() > 1)
    {
        Value vMax = params[1];
        ConvertTo<double>(vMax);
        nMax = (int)vMax.get_real();
    }

    CNameDB dbName("r");
    Array oRes;

    //vector<pair<vector<unsigned char>, CDiskTxPos> > nameScan;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!dbName.ScanNames(vchName, nMax, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    //pair<vector<unsigned char>, CDiskTxPos> pairScan;
    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        Object oName;
        string name = stringFromVch(pairScan.first);
        oName.push_back(Pair("name", name));
        //vector<unsigned char> vchValue;
        CTransaction tx;
        CNameIndex txName = pairScan.second;
        CDiskTxPos txPos = txName.txPos;
        //CDiskTxPos txPos = pairScan.second;
        //int nHeight = GetTxPosHeight(txPos);
        int nHeight = txName.nHeight;
        vector<unsigned char> vchValue = txName.vValue;
        if (!txPos.IsNull() && !tx.ReadFromDisk(txPos))
        {
            string value = stringFromVch(vchValue);
            //string strAddress = "";
            //GetNameAddress(tx, strAddress);
            oName.push_back(Pair("value", value));
            //oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            //oName.push_back(Pair("address", strAddress));
        }
        oRes.push_back(oName);
    }

    if (NAME_DEBUG) {
        dbName.test();
    }
    return oRes;
}

Value name_firstupdate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 5)
        throw runtime_error(
                "name_firstupdate <name> <rand> [<tx>] <value> [<toaddress>]\n"
                "Perform a first update after a name_new reservation.\n"
                "Note that the first update will go into a block 2 blocks after the name_new, at the soonest."
                + HelpRequiringPassphrase());
    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchRand = ParseHex(params[1].get_str());
    vector<unsigned char> vchValue;

    if (params.size() == 3)
    {
        vchValue = vchFromValue(params[2]);
    }
    else
    {
        vchValue = vchFromValue(params[3]);
    }

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    CRITICAL_BLOCK(cs_main)
    {
        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            error("name_firstupdate() : there are %d pending operations on that name, including %s",
                    mapNamePending[vchName].size(),
                    mapNamePending[vchName].begin()->GetHex().c_str());
            throw runtime_error("there are pending operations on that name");
        }
    }

    {
        CNameDB dbName("r");
        CTransaction tx;
        if (GetTxOfName(dbName, vchName, tx) && !tx.IsGameTx())
        {
            error("name_firstupdate() : this name is already active with tx %s",
                    tx.GetHash().GetHex().c_str());
            throw runtime_error("this name is already active");
        }
    }

    CScript scriptPubKeyOrig;
    if (params.size () == 5)
    {
        const std::string strAddress = params[4].get_str ();
        uint160 hash160;
        bool isValid = AddressToHash160 (strAddress, hash160);
        if (!isValid)
            throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY,
                                "Invalid huntercoin address");
        scriptPubKeyOrig.SetBitcoinAddress (strAddress);
    }
    else
    {
        vector<unsigned char> vchPubKey = pwalletMain->GetKeyFromKeyPool ();
        scriptPubKeyOrig.SetBitcoinAddress (vchPubKey);
    }

    CRITICAL_BLOCK(cs_main)
    {
        EnsureWalletIsUnlocked();

        // Make sure there is a previous NAME_NEW tx on this name
        // and that the random value matches
        uint256 wtxInHash;
        if (params.size() == 3)
        {
            if (!mapMyNames.count(vchName))
            {
                throw runtime_error("could not find a coin with this name, try specifying the name_new transaction id");
            }
            wtxInHash = mapMyNames[vchName];
        }
        else
        {
            wtxInHash.SetHex(params[2].get_str());
        }

        if (!pwalletMain->mapWallet.count(wtxInHash))
        {
            throw runtime_error("previous transaction is not in the wallet");
        }

        CScript scriptPubKey;
        scriptPubKey << OP_NAME_FIRSTUPDATE << vchName << vchRand << vchValue << OP_2DROP << OP_2DROP;
        scriptPubKey += scriptPubKeyOrig;

        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        vector<unsigned char> vchHash;
        bool found = false;
        BOOST_FOREACH(CTxOut& out, wtxIn.vout)
        {
            vector<vector<unsigned char> > vvch;
            int op;
            if (DecodeNameScript(out.scriptPubKey, op, vvch)) {
                if (op != OP_NAME_NEW)
                    throw runtime_error("previous transaction wasn't a name_new");
                vchHash = vvch[0];
                found = true;
            }
        }

        if (!found)
        {
            throw runtime_error("previous tx on this name is not a name tx");
        }

        vector<unsigned char> vchToHash(vchRand);
        vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
        uint160 hash = Hash160(vchToHash);
        if (uint160(vchHash) != hash)
        {
            throw runtime_error("previous tx used a different random value");
        }

        int64 nNetFee = GetNetworkFee(pindexBest->nHeight);
        // Round up to CENT
        nNetFee += CENT - 1;
        nNetFee = (nNetFee / CENT) * CENT;

        const int64 nCoinAmount = GetNameCoinAmount (pindexBest->nHeight, true);

        std::string strError;
        strError = SendMoneyWithInputTx (scriptPubKey, nCoinAmount,
                                         nNetFee, wtxIn, wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return wtx.GetHash().GetHex();
}

Value name_update(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
        throw runtime_error(
                "name_update <name> <value> [<toaddress>]\nUpdate and possibly transfer a name"
                + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    CScript scriptPubKey;
    scriptPubKey << OP_NAME_UPDATE << vchName << vchValue << OP_2DROP << OP_DROP;

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
    {
        if (mapNamePending.count(vchName) && mapNamePending[vchName].size())
        {
            error("name_update() : there are %d pending operations on that name, including %s",
                    mapNamePending[vchName].size(),
                    mapNamePending[vchName].begin()->GetHex().c_str());
            throw runtime_error("there are pending operations on that name");
        }

        EnsureWalletIsUnlocked();

        CTransaction tx;
        {
          CNameDB dbName("r");
          if (!GetTxOfName(dbName, vchName, tx))
            throw runtime_error("could not find a coin with this name");
        }

        if (tx.IsGameTx ())
          throw std::runtime_error ("this player is dead");

        const uint256 wtxInHash = tx.GetHash ();
        if (!pwalletMain->mapWallet.count(wtxInHash))
        {
            error("name_update() : this coin is not in your wallet %s",
                    wtxInHash.GetHex().c_str());
            throw runtime_error("this coin is not in your wallet");
        }

        CScript scriptPubKeyOrig;
        if (params.size() == 3)
        {
            string strAddress = params[2].get_str();
            uint160 hash160;
            bool isValid = AddressToHash160(strAddress, hash160);
            if (!isValid)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid huntercoin address");
            scriptPubKeyOrig.SetBitcoinAddress(strAddress);
        }
        else
        {
            uint160 hash160;
            GetNameAddress(tx, hash160);
            scriptPubKeyOrig.SetBitcoinAddress(hash160);
        }
        scriptPubKey += scriptPubKeyOrig;

        /* Find amount locked in this name.  */
        const int nTxOut = IndexOfNameOutput (tx);
        const int64 nCoinAmount = tx.vout[nTxOut].nValue;

        CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
        string strError;
        strError = SendMoneyWithInputTx (scriptPubKey, nCoinAmount,
                                         0, wtxIn, wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return wtx.GetHash().GetHex();
}

Value name_new(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
                "name_new <name>"
                + HelpRequiringPassphrase());

    const std::string& name = params[0].get_str ();
    if (!IsValidPlayerName (name))
      throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid player name");

    const vchType vchName = vchFromString (name);

    CWalletTx wtx;
    wtx.nVersion = NAMECOIN_TX_VERSION;

    uint64 rand = GetRand((uint64)-1);
    const vchType vchRand = CBigNum(rand).getvch();
    vchType vchToHash(vchRand);
    vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
    const uint160 hash = Hash160(vchToHash);

    const vchType vchPubKey = pwalletMain->GetKeyFromKeyPool();
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetBitcoinAddress(vchPubKey);
    CScript scriptPubKey;
    scriptPubKey << OP_NAME_NEW << hash << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

    CRITICAL_BLOCK(cs_main)
    {
        EnsureWalletIsUnlocked();

        /* We could send NAMENEW_COIN_AMOUNT instead and let name_firstupdate
           add the missing funds, but this will lead to problems while all
           clients update and older ones expect an exact amount.  */
        const int64 nCoinAmount = GetNameCoinAmount (pindexBest->nHeight, true);

        string strError = pwalletMain->SendMoney (scriptPubKey, nCoinAmount,
                                                  wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        mapMyNames[vchName] = wtx.GetHash();
    }

    printf("name_new : name=%s, rand=%s, tx=%s\n", stringFromVch(vchName).c_str(), HexStr(vchRand).c_str(), wtx.GetHash().GetHex().c_str());

    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(HexStr(vchRand));
    return res;
}

/* Implement name operations for createrawtransaction.  */
void
AddRawTxNameOperation (CTransaction& tx, const Object& obj)
{
  Value val = find_value (obj, "op");
  if (val.type () != str_type)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "Missing op key.");
  const std::string op = val.get_str ();

  if (op != "name_update")
    throw std::runtime_error ("Only name_update is implemented"
                              " for raw transactions at the moment.");

  val = find_value (obj, "name");
  if (val.type () != str_type)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "Missing name key.");
  const std::string name = val.get_str ();
  const std::vector<unsigned char> vchName = vchFromString (name);

  val = find_value (obj, "value");
  if (val.type () != str_type)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "Missing value key.");
  const std::string value = val.get_str ();

  val = find_value (obj, "address");
  if (val.type () != str_type)
    throw JSONRPCError (RPC_INVALID_PARAMETER, "Missing address key.");
  const std::string address = val.get_str ();
  if (!IsValidBitcoinAddress (address))
    {
      std::ostringstream msg;
      msg << "Invalid Huntercoin address: " << address;
      throw JSONRPCError (RPC_INVALID_ADDRESS_OR_KEY, msg.str ());
    }

  tx.nVersion = NAMECOIN_TX_VERSION;

  /* Find the transaction input to add.  */

  int64 nCoinAmount = -1;
  CRITICAL_BLOCK(cs_main)
  {
    CNameDB dbName("r");
    CTransaction prevTx;
    if (!GetTxOfName (dbName, vchName, prevTx))
      throw std::runtime_error ("could not find a coin with this name");
    const uint256 prevTxHash = prevTx.GetHash();
    const int nTxOut = IndexOfNameOutput (prevTx);

    CTxIn in(COutPoint(prevTxHash, nTxOut));
    tx.vin.push_back (in);

    nCoinAmount = prevTx.vout[nTxOut].nValue;
  }
  assert (nCoinAmount >= 0);

  /* Construct the transaction output.  */

  CScript scriptPubKeyOrig;
  scriptPubKeyOrig.SetBitcoinAddress (address);

  CScript scriptPubKey;
  scriptPubKey << OP_NAME_UPDATE << vchName << vchFromString (value)
               << OP_2DROP << OP_DROP;
  scriptPubKey += scriptPubKeyOrig;

  CTxOut out(nCoinAmount, scriptPubKey);
  tx.vout.push_back (out);
}

static Value
name_pending (const Array& params, bool fHelp)
{
  if (fHelp || params.size () != 0)
    throw runtime_error(
      "name_pending\n"
      "List all pending name operations known of.\n");

  Array res;

  CRITICAL_BLOCK (cs_main)
    {
      std::map<vchType, std::set<uint256> >::const_iterator i;
      for (i = mapNamePending.begin (); i != mapNamePending.end (); ++i)
        {
          if (i->second.empty ())
            continue;

          const std::string name = stringFromVch (i->first);

          for (std::set<uint256>::const_iterator j = i->second.begin ();
               j != i->second.end (); ++j)
            {
              CTransaction tx;
              uint256 hashBlock;
              if (!GetTransaction (*j, tx, hashBlock))
                {
                  printf ("name_pending: failed to GetTransaction of hash %s\n",
                          j->GetHex ().c_str ());
                  continue;
                }

              int op, nOut;
              std::vector<vchType> vvch;
              if (!DecodeNameTx (tx, op, nOut, vvch))
                {
                  printf ("name_pending: failed to find name output in tx %s\n",
                          j->GetHex ().c_str ());
                  continue;
                }

              /* Decode the name operation.  */
              std::string value;
              std::string opString;
              switch (op)
                {
                case OP_NAME_FIRSTUPDATE:
                  assert (vvch.size () == 3);
                  opString = "name_firstupdate";
                  value = stringFromVch (vvch[2]);
                  break;

                case OP_NAME_UPDATE:
                  assert (vvch.size () == 2);
                  opString = "name_update";
                  value = stringFromVch (vvch[1]);
                  break;

                default:
                  printf ("name_pending: unexpected op code %d for tx %s\n",
                          op, j->GetHex ().c_str ());
                  continue;
                }

              /* See if it is owned by the wallet user.  */
              const CTxOut& txout = tx.vout[nOut];
              const bool isMine = IsMyName (txout);

              /* Construct the JSON output.  */
              Object obj;
              obj.push_back (Pair ("name", name));
              obj.push_back (Pair ("txid", j->GetHex ()));
              obj.push_back (Pair ("op", opString));
              obj.push_back (Pair ("value", value));
              obj.push_back (Pair ("ismine", isMine));
              res.push_back (obj);
            }
        }
    }

  return res;
}

Value game_getstate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
                "game_getstate [height]\n"
                "Returns game state, either the most recent one or at given height (-1 = initial state, 0 = state after genesis block, k = state after k-th block for k>0)\n"
                );

    int64 height = nBestHeight;

    if (params.size() > 0)
    {
        height = params[0].get_int64();
    }
    else if (IsInitialBlockDownload())
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "huntercoin is downloading blocks...");

    if (height < -1 || height > nBestHeight)
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid height specified");

    Game::GameState state;

    CRITICAL_BLOCK(cs_main)
    {
        CBlockIndex* pindex;
        if (height == -1)
            pindex = NULL;
        else
        {
            pindex = pindexBest;
            while (pindex && pindex->nHeight > height)
                pindex = pindex->pprev;
            if (!pindex || pindex->nHeight != height)
                throw JSONRPCError(RPC_DATABASE_ERROR, "Cannot find block at specified height");
        }

        DatabaseSet dbset("r");
        if (!GetGameState (dbset, pindex, state))
            throw JSONRPCError(RPC_DATABASE_ERROR, "Cannot compute game state at specified height");
    }

    return state.ToJsonValue();
}

/* Wait for the next block to be found and processed (blocking in a waiting
   thread) and return the new state when it is done.  */
Value game_waitforchange (const Array& params, bool fHelp)
{
  if (fHelp || params.size () > 1)
    throw runtime_error (
            "game_waitforchange [blockHash]\n"
            "Wait for a change in the best chain (a new block being found)"
            " and return the new game state.  If blockHash is given, wait"
            " until a block with different hash is found.\n");

  if (IsInitialBlockDownload ())
    throw JSONRPCError (RPC_CLIENT_IN_INITIAL_DOWNLOAD,
                        "huntercoin is downloading blocks...");

  uint256 lastHash;
  if (params.size () > 0)
    lastHash = ParseHashV (params[0], "blockHash");
  else
    lastHash = hashBestChain;

  boost::unique_lock<boost::mutex> lock(mut_currentState);
  while (true)
    {
      /* Atomically check whether we have found a new best block and return
         it if that's the case.  We use a lock on cs_main in order to
         prevent race conditions.  */
      CRITICAL_BLOCK(cs_main)
        {
          if (lastHash != hashBestChain)
            {
              const Game::GameState& state = GetCurrentGameState ();
              return state.ToJsonValue();
            }
        }

      /* Wait on the condition variable.  */
      cv_stateChange.wait (lock);
    }
}

Value game_getplayerstate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
                "game_getplayerstate <name> [height]\n"
                "Returns player state. Similar to game_getstate, but filters the name.\n"
                );

    int64 height = nBestHeight;

    if (params.size() > 1)
    {
        height = params[1].get_int64();
    }
    else if (IsInitialBlockDownload())
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "huntercoin is downloading blocks...");


    if (height < -1 || height > nBestHeight)
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid height specified");

    Game::GameState state;

    CRITICAL_BLOCK(cs_main)
    {
        CBlockIndex* pindex;
        if (height == -1)
            pindex = NULL;
        else
        {
            pindex = pindexBest;
            while (pindex && pindex->nHeight > height)
                pindex = pindex->pprev;
            if (!pindex || pindex->nHeight != height)
                throw JSONRPCError(RPC_DATABASE_ERROR, "Cannot find block at specified height");
        }

        DatabaseSet dbset("r");
        if (!GetGameState (dbset, pindex, state))
            throw JSONRPCError(RPC_DATABASE_ERROR, "Cannot compute game state at specified height");
    }

    Game::PlayerID player_name = params[0].get_str();
    std::map<Game::PlayerID, Game::PlayerState>::const_iterator mi = state.players.find(player_name);
    if (mi == state.players.end())
        throw JSONRPCError(RPC_DATABASE_ERROR, "No such player");

    int crown_index = player_name == state.crownHolder.player ? state.crownHolder.index : -1;
    return mi->second.ToJsonValue(crown_index);
}

/* Give access to the game's shortest path algorithm to calculate
   paths from one coordinate to another one.  */
Value
game_getpath (const Array& params, bool fHelp)
{
  if (fHelp || params.size () != 2)
    throw runtime_error ("game_getpath [fromX,fromY] [toX,toY]\n"
                         "Return a set of way points that travels in a\n"
                         "shortest path between the given coordinates.\n");

  if (params[0].type () != array_type || params[1].type () != array_type)
    throw runtime_error ("arguments must be arrays");

  const Array from = params[0].get_array ();
  const Array to = params[1].get_array ();

  if (from.size () != 2 || to.size () != 2)
    throw runtime_error ("invalid coordinates given");

  const Game::Coord fromC(from[0].get_int (), from[1].get_int ());
  const Game::Coord toC(to[0].get_int (), to[1].get_int ());

  std::vector<Game::Coord> path = FindPath (fromC, toC);

  Array res;
  bool first = true;
  BOOST_FOREACH(const Game::Coord& c, path)
    {
      if (first)
        {
          first = false;
          continue;
        }

      res.push_back (c.x);
      res.push_back (c.y);
    }

  return res;
}

void UnspendInputs(CWalletTx& wtx)
{
    set<CWalletTx*> setCoins;
    BOOST_FOREACH(const CTxIn& txin, wtx.vin)
    {
        if (!pwalletMain->IsMine(txin))
        {
            printf("UnspendInputs(): !mine %s", txin.ToString().c_str());
            continue;
        }
        CWalletTx& prev = pwalletMain->mapWallet[txin.prevout.hash];
        int nOut = txin.prevout.n;

        printf("UnspendInputs(): %s:%d spent %d\n", prev.GetHash().ToString().c_str(), nOut, prev.IsSpent(nOut));

        if (nOut >= prev.vout.size())
            throw runtime_error("CWalletTx::MarkSpent() : nOut out of range");
        prev.vfSpent.resize(prev.vout.size());
        if (prev.vfSpent[nOut])
        {
            prev.vfSpent[nOut] = false;
            prev.fAvailableCreditCached = false;
            prev.WriteToDisk();
        }
    }
#ifdef GUI
    pwalletMain->NotifyTransactionChanged(pwalletMain, wtx.GetHash(), CT_DELETED);
#endif
}

Value deletetransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
                "deletetransaction <txid>\nNormally used when a transaction cannot be confirmed due to a double spend.\nRestart the program after executing this call.\n"
                );

    if (params.size() != 1)
      throw runtime_error("missing txid");
    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
    {
      uint256 hash;
      hash.SetHex(params[0].get_str());
      if (!pwalletMain->mapWallet.count(hash))
        throw runtime_error("transaction not in wallet");

      if (!mapTransactions.count(hash))
      {
        //throw runtime_error("transaction not in memory - is already in blockchain?");
        CTransaction tx;
        uint256 hashBlock = 0;
        if (GetTransaction(hash, tx, hashBlock /*, true*/) && hashBlock != 0)
          throw runtime_error("transaction is already in blockchain");
      }
      CWalletTx wtx = pwalletMain->mapWallet[hash];
      UnspendInputs(wtx);

      // We are not removing from mapTransactions because this can cause memory corruption
      // during mining.  The user should restart to clear the tx from memory.
      wtx.RemoveFromMemoryPool();
      pwalletMain->EraseFromWallet(wtx.GetHash());
      vector<unsigned char> vchName;
      if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName)) {
        printf("deletetransaction() : remove from pending");
        mapNamePending[vchName].erase(wtx.GetHash());
      }
      return "success, please restart program to clear memory";
    }
}

void rescanfornames()
{
    printf("Scanning blockchain for names to create fast index...\n");

    /* The database should already be created although empty.  */

    CNameDB dbName("r+");
    dbName.ReconstructNameIndex();
}

Value name_clean(const Array& params, bool fHelp)
{
    if (fHelp || params.size())
        throw runtime_error("name_clean\nClean unsatisfiable transactions from the wallet - including name_update on an already taken name\n");

    CRITICAL_BLOCK(cs_main)
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
    {
        map<uint256, CWalletTx> mapRemove;

        printf("-----------------------------\n");

        {
            DatabaseSet dbset("r");
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;
                vchType vchName;
                if (wtx.GetDepthInMainChain () < 1
                    && IsConflictedTx (dbset, wtx, vchName))
                {
                    uint256 hash = wtx.GetHash();
                    mapRemove[hash] = wtx;
                }
            }
        }

        bool fRepeat = true;
        while (fRepeat)
        {
            fRepeat = false;
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
            {
                CWalletTx& wtx = item.second;
                BOOST_FOREACH(const CTxIn& txin, wtx.vin)
                {
                    uint256 hash = wtx.GetHash();

                    // If this tx depends on a tx to be removed, remove it too
                    if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash))
                    {
                        mapRemove[hash] = wtx;
                        fRepeat = true;
                    }
                }
            }
        }

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove)
        {
            CWalletTx& wtx = item.second;

            UnspendInputs(wtx);
            wtx.RemoveFromMemoryPool();
            pwalletMain->EraseFromWallet(wtx.GetHash());
            vector<unsigned char> vchName;
            if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName))
            {
                string name = stringFromVch(vchName);
                printf("name_clean() : erase %s from pending of name %s", 
                        wtx.GetHash().GetHex().c_str(), name.c_str());
                if (!mapNamePending[vchName].erase(wtx.GetHash()))
                    error("name_clean() : erase but it was not pending");
            }
            wtx.print();
        }

        printf("-----------------------------\n");
    }

    return true;
}

bool CNameDB::test()
{
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    loop
    {
        // Read next record
        CDataStream ssKey;
        CDataStream ssValue;
        int ret = ReadAtCursor(pcursor, ssKey, ssValue);
        if (ret == DB_NOTFOUND)
            break;
        else if (ret != 0)
            return false;

        // Unserialize
        string strType;
        ssKey >> strType;
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            string strName = stringFromVch(vchName);
            vector<CDiskTxPos> vtxPos;
            ssValue >> vtxPos;
            if (NAME_DEBUG)
              printf("NAME %s : ", strName.c_str());
            BOOST_FOREACH(CDiskTxPos& txPos, vtxPos) {
                txPos.print();
                if (NAME_DEBUG)
                  printf(" @ %d, ", GetTxPosHeight(txPos));
            }
            if (NAME_DEBUG)
              printf("\n");
        }
    }
    pcursor->close();
}

bool CNameDB::ScanNames(
        const vector<unsigned char>& vchName,
        int nMax,
        vector<pair<vector<unsigned char>, CNameIndex> >& nameScan)
        //vector<pair<vector<unsigned char>, CDiskTxPos> >& nameScan)
{
    Dbc* pcursor = GetCursor();
    if (!pcursor)
        return false;

    unsigned int fFlags = DB_SET_RANGE;
    loop
    {
        // Read next record
        CDataStream ssKey;
        if (fFlags == DB_SET_RANGE)
            ssKey << make_pair(string("namei"), vchName);
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
        if (strType == "namei")
        {
            vector<unsigned char> vchName;
            ssKey >> vchName;
            string strName = stringFromVch(vchName);
            //vector<CDiskTxPos> vtxPos;
            vector<CNameIndex> vtxPos;
            ssValue >> vtxPos;
            //CDiskTxPos txPos;
            CNameIndex txPos;
            if (!vtxPos.empty())
            {
                txPos = vtxPos.back();
            }
            nameScan.push_back(make_pair(vchName, txPos));
        }

        if (nameScan.size() >= nMax)
            break;
    }
    pcursor->close();
    return true;
}

/* Analyse the UTXO set.  This possibly takes a very long time.  */
static Value
analyseutxo (const Array& params, bool fHelp)
{
  if (fHelp || params.size() > 0)
    throw std::runtime_error (
          "analyseutxo\n"
          "Look through the UTXO and construct certain data about it.");

  CTxDB txdb("r");
  CCriticalBlock lock(cs_main);

  unsigned txCnt = 0;
  unsigned txoCnt = 0;
  unsigned totalTx = 0;
  unsigned totalTxo = 0;
  int64_t amount = 0;
  const CBlockIndex* pInd = pindexBest;
  const unsigned startingHeight = pInd->nHeight;
  for (; pInd; pInd = pInd->pprev)
    {
      printf ("Analyse UTXO at block height %d...\n", pInd->nHeight);
      std::vector<const CTransaction*> vTxs;

      CBlock block;
      block.ReadFromDisk (pInd);

      for (unsigned i = 0; i < block.vtx.size (); ++i)
        vTxs.push_back (&block.vtx[i]);
      for (unsigned i = 0; i < block.vgametx.size (); ++i)
        vTxs.push_back (&block.vgametx[i]);

      totalTx += vTxs.size ();
      for (unsigned i = 0; i < vTxs.size (); ++i)
        {
          CTxIndex txindex;
          if (!txdb.ReadTxIndex (vTxs[i]->GetHash (), txindex))
            throw std::runtime_error ("ReadTxIndex failed.");

          bool hasUnspent = false;
          totalTxo += vTxs[i]->vout.size ();
          for (unsigned j = 0; j < vTxs[i]->vout.size (); ++j)
            if (!txindex.IsSpent (j))
              {
                hasUnspent = true;
                ++txoCnt;
                amount += vTxs[i]->vout[j].nValue;
              }
          if (hasUnspent)
            ++txCnt;
        }

      /* Since this is an async RPC call, give the main thread a chance
         to interrupt this thread in case the server is going to shut down.  */
      boost::this_thread::interruption_point ();
    }

  /* Also calculate total number of coins on the map, so that we get the total
     money supply and can check it.  */
  const Game::GameState& state = GetCurrentGameState ();
  assert (state.nHeight == startingHeight);
  const int64 onMap = state.GetCoinsOnMap ();
  const int64 lostCoins = state.lostCoins;
  const int64 rewards = pindexBest->GetTotalRewards ();

  /* Construct the result.  */

  Object res;
  res.push_back (Pair ("height", static_cast<int> (startingHeight)));

  Object subobj;
  subobj.push_back (Pair ("tx", static_cast<int> (txCnt)));
  subobj.push_back (Pair ("txo", static_cast<int> (txoCnt)));
  res.push_back (Pair ("unspent", subobj));

  subobj.clear ();
  subobj.push_back (Pair ("tx", static_cast<int> (totalTx)));
  subobj.push_back (Pair ("txo", static_cast<int> (totalTxo)));
  res.push_back (Pair ("total", subobj));

  subobj.clear ();
  subobj.push_back (Pair ("utxo", ValueFromAmount (amount)));
  subobj.push_back (Pair ("map", ValueFromAmount (onMap)));
  subobj.push_back (Pair ("total", ValueFromAmount (amount + onMap)));
  res.push_back (Pair ("moneysupply", subobj));

  subobj.clear ();
  subobj.push_back (Pair ("rewards", ValueFromAmount (rewards)));
  subobj.push_back (Pair ("lost", ValueFromAmount (lostCoins)));
  subobj.push_back (Pair ("total", ValueFromAmount (rewards - lostCoins)));
  res.push_back (Pair ("expected", subobj));
  res.push_back (Pair ("check", amount + onMap == rewards - lostCoins));

  return res;
}

bool CNameDB::ReconstructNameIndex()
{
    CTxDB txdb("r");
    vchType vchName;
    vchType vchValue;
    CTxIndex txindex;
    CBlockIndex* pindex = pindexGenesisBlock;
    CRITICAL_BLOCK(pwalletMain->cs_mapWallet)
    {
        while (pindex)
        {  
            TxnBegin();
            CBlock block;
            block.ReadFromDisk(pindex);
            BOOST_FOREACH(CTransaction& tx, block.vtx)
            {
                if (tx.nVersion != NAMECOIN_TX_VERSION)
                    continue;

                if(!GetNameOfTx(tx, vchName))
                    continue;

                if(!GetValueOfNameTx(tx, vchValue))
                    continue;

                uint256 hash = tx.GetHash();
                if (!txdb.ReadDiskTx(hash, tx, txindex))
                {
                    printf("Rescanfornames() : ReadDiskTx failed for tx %s\n", hash.ToString().c_str());
                    continue;
                }

                vector<CNameIndex> vtxPos;
                if (ExistsName(vchName))
                    if (!ReadName(vchName, vtxPos))
                        return error("Rescanfornames() : failed to read from name DB");
                CNameIndex txPos2;
                txPos2.nHeight = pindex->nHeight;
                txPos2.vValue = vchValue;
                txPos2.txPos = txindex.pos;
                vtxPos.push_back(txPos2);
                if (!WriteName(vchName, vtxPos))
                    return error("Rescanfornames() : failed to write to name DB");
            }
            BOOST_FOREACH(CTransaction& tx, block.vgametx)
            {
                uint256 hash = tx.GetHash();
                if (!txdb.ReadDiskTx(hash, tx, txindex))
                {
                    printf("Rescanfornames() : ReadDiskTx failed for tx %s\n", hash.ToString().c_str());
                    continue;
                }

                for (int i = 0; i < tx.vin.size(); i++)
                {
                    if (tx.vin[i].prevout.IsNull())
                        continue;

                    int nout = tx.vin[i].prevout.n;

                    CTransaction txPrev;
                    uint256 hashBlock = 0;
                    if (!GetTransaction(tx.vin[i].prevout.hash, txPrev, hashBlock))
                        return error("Rescanfornames() : GetTransaction failed");

                    if (nout >= txPrev.vout.size())
                        continue;
                    CTxOut prevout = txPrev.vout[nout];

                    int prevOp;
                    vector<vchType> vvchPrevArgs;

                    if (!DecodeNameScript(prevout.scriptPubKey, prevOp, vvchPrevArgs) || prevOp == OP_NAME_NEW)
                        continue;

                    vector<CNameIndex> vtxPos;
                    if (ExistsName(vvchPrevArgs[0]))
                    {
                        if (!ReadName(vvchPrevArgs[0], vtxPos))
                            return error("Rescanfornames() : failed to read from name DB");
                    }
                    CNameIndex txPos2;
                    txPos2.nHeight = pindex->nHeight;
                    txPos2.vValue = vchFromString(VALUE_DEAD);
                    txPos2.txPos = txindex.pos;
                    vtxPos.push_back(txPos2);
                    if (!WriteName(vvchPrevArgs[0], vtxPos))
                        return error("Rescanfornames() : failed to write to name DB");
                }
            }
            pindex = pindex->pnext;
            TxnCommit();
        }
    }
}

CHooks* InitHook()
{
    mapCallTable.insert(make_pair("analyseutxo", &analyseutxo));
    mapCallTable.insert(make_pair("name_new", &name_new));
    mapCallTable.insert(make_pair("name_update", &name_update));
    mapCallTable.insert(make_pair("name_firstupdate", &name_firstupdate));
    mapCallTable.insert(make_pair("name_list", &name_list));
    mapCallTable.insert(make_pair("name_scan", &name_scan));
    mapCallTable.insert(make_pair("name_filter", &name_filter));
    mapCallTable.insert(make_pair("name_show", &name_show));
    mapCallTable.insert(make_pair("name_history", &name_history));
    mapCallTable.insert(make_pair("name_debug", &name_debug));
    mapCallTable.insert(make_pair("name_debug1", &name_debug1));
    mapCallTable.insert(make_pair("name_clean", &name_clean));
    mapCallTable.insert(make_pair("name_pending", &name_pending));
    mapCallTable.insert(make_pair("sendtoname", &sendtoname));
    mapCallTable.insert(make_pair("game_getstate", &game_getstate));
    mapCallTable.insert(make_pair("game_waitforchange", &game_waitforchange));
    mapCallTable.insert(make_pair("game_getplayerstate", &game_getplayerstate));
    mapCallTable.insert(make_pair("game_getpath", &game_getpath));
    mapCallTable.insert(make_pair("deletetransaction", &deletetransaction));
    setCallAsync.insert("game_waitforchange");
    hashGenesisBlock = hashHuntercoinGenesisBlock[fTestNet ? 1 : 0];
    printf("Setup huntercoin genesis block %s\n", hashGenesisBlock.GetHex().c_str());
    return new CHuntercoinHooks();
}

bool DecodeNameScript(const CScript& script, int& op, vector<vchType> &vvch)
{
    CScript::const_iterator pc = script.begin();
    return DecodeNameScript(script, op, vvch, pc);
}

bool DecodeNameScript(const CScript& script, int& op,
                      vector<vchType> &vvch, CScript::const_iterator& pc)
{
    opcodetype opcode;
    if (!script.GetOp(pc, opcode))
        return false;
    if (opcode < OP_1 || opcode > OP_16)
        return false;

    op = opcode - OP_1 + 1;

    for (;;) {
        vector<unsigned char> vch;
        if (!script.GetOp(pc, opcode, vch))
            return false;
        if (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
            break;
        if (!(opcode >= 0 && opcode <= OP_PUSHDATA4))
            return false;
        vvch.push_back(vch);
    }

    // move the pc to after any DROP or NOP
    while (opcode == OP_DROP || opcode == OP_2DROP || opcode == OP_NOP)
    {
        if (!script.GetOp(pc, opcode))
            break;
    }

    pc--;

    if ((op == OP_NAME_NEW && vvch.size() == 1) ||
            (op == OP_NAME_FIRSTUPDATE && vvch.size() == 3) ||
            (op == OP_NAME_UPDATE && vvch.size() == 2))
        return true;
    return error("invalid number of arguments for name op");
}

bool DecodeNameTx(const CTransaction& tx, int& op, int& nOut,
                  vector<vchType>& vvch)
{
    bool found = false;

    for (int i = 0 ; i < tx.vout.size() ; i++)
    {
        const CTxOut& out = tx.vout[i];
        vector<vector<unsigned char> > vvchRead;
        if (DecodeNameScript(out.scriptPubKey, op, vvchRead))
        {
            // If more than one name op, fail
            if (found)
            {
                vvch.clear();
                return false;
            }
            vvch = vvchRead;
            nOut = i;
            found = true;
        }
    }

    return found;
}

int64 GetNameNetFee(const CTransaction& tx)
{
    int64 nFee = 0;

    for (int i = 0 ; i < tx.vout.size() ; i++)
    {
        const CTxOut& out = tx.vout[i];
        if (out.scriptPubKey.size() == 1 && out.scriptPubKey[0] == OP_RETURN)
        {
            nFee += out.nValue;
        }
    }

    return nFee;
}

bool GetValueOfNameTx(const CTransaction& tx, vector<unsigned char>& value)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if (!DecodeNameTx(tx, op, nOut, vvch))
        return false;

    switch (op)
    {
        case OP_NAME_NEW:
            return false;
        case OP_NAME_FIRSTUPDATE:
            value = vvch[2];
            return true;
        case OP_NAME_UPDATE:
            value = vvch[1];
            return true;
        default:
            return false;
    }
}

int IndexOfNameOutput(const CTransaction& tx)
{
    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        if (tx.IsGameTx())
            throw runtime_error("IndexOfNameOutput() : name output not found (game transaction was supplied)");
        throw runtime_error("IndexOfNameOutput() : name output not found");
    }
    return nOut;
}

void CHuntercoinHooks::AddToWallet(CWalletTx& wtx)
{
}

bool CHuntercoinHooks::IsMine(const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        error("IsMine() hook : no output out script in name tx %s\n", tx.ToString().c_str());
        return false;
    }

    const CTxOut& txout = tx.vout[nOut];
    if (IsMyName (txout))
        return true;
    return false;
}

bool CHuntercoinHooks::IsMine(const CTransaction& tx, const CTxOut& txout, bool ignore_name_new /* = false*/)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;
 
    if (!DecodeNameScript(txout.scriptPubKey, op, vvch))
        return false;

    if (ignore_name_new && op == OP_NAME_NEW)
        return false;

    if (IsMyName (txout))
        return true;
    return false;
}

void
CHuntercoinHooks::AcceptToMemoryPool (DatabaseSet& dbset,
                                      const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return;

    if (tx.vout.size() < 1)
    {
        error("AcceptToMemoryPool() : no output in name tx %s\n", tx.ToString().c_str());
        return;
    }

    vector<vchType> vvch;

    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
    {
        error("AcceptToMemoryPool() : no output out script in name tx %s", tx.ToString().c_str());
        return;
    }

    if (op != OP_NAME_NEW)
    {
        CRITICAL_BLOCK(cs_main)
            mapNamePending[vvch[0]].insert(tx.GetHash());
    }
}

void CHuntercoinHooks::RemoveFromMemoryPool(const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return;

    if (tx.vout.size() < 1)
        return;

    vector<vector<unsigned char> > vvch;

    int op;
    int nOut;

    if (!DecodeNameTx(tx, op, nOut, vvch))
        return;

    if (op != OP_NAME_NEW)
    {
        CRITICAL_BLOCK(cs_main)
        {
            std::map<std::vector<unsigned char>, std::set<uint256> >::iterator mi = mapNamePending.find(vvch[0]);
            if (mi != mapNamePending.end())
                mi->second.erase(tx.GetHash());
        }
    }
}

bool GetNameOfTx(const CTransaction& tx, vector<unsigned char>& name)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;
    vector<vector<unsigned char> > vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("GetNameOfTx() : could not decode a name tx");

    switch (op)
    {
        case OP_NAME_FIRSTUPDATE:
        case OP_NAME_UPDATE:
            name = vvchArgs[0];
            return true;
    }
    return false;
}

bool
IsConflictedTx (DatabaseSet& dbset, const CTransaction& tx, vchType& name)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return false;
    vector<vchType> vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("IsConflictedTx() : could not decode a name tx");
    int nPrevHeight;
    int nDepth;
    int64 nNetFee;

    switch (op)
    {
        case OP_NAME_FIRSTUPDATE:
            name = vvchArgs[0];
            if (!NameAvailable (dbset, name))
                return true;
    }
    return false;
}

// Detect if player is dead by checking wheter his name is spent by a game transaction
bool
IsPlayerDead (const CWalletTx &nameTx, const CTxIndex &txindex)
{
  if (nameTx.IsGameTx ())
      return true;

  const unsigned nOut = IndexOfNameOutput (nameTx);
  if (nOut < txindex.GetOutputCount ()
      && txindex.GetSpent (nOut) == CTxIndex::SPENT_GAMETX)
    return true;

  return false;
}

static bool
ConnectInputsGameTx (DatabaseSet& dbset,
                     const std::map<uint256, CTxIndex>& mapTestPool,
                     const CTransaction& tx, const CBlockIndex* pindexBlock,
                     const CDiskTxPos& txPos)
{
    if (!tx.IsGameTx())
        return error("ConnectInputsGameTx called for non-game tx");

    for (int i = 0; i < tx.vin.size(); i++)
    {
        if (tx.vin[i].prevout.IsNull ())
          continue;

        vchType name;
        if (!IsPlayerDeathInput (tx.vin[i], name))
          return error ("ConnectInputsGameTx: prev is no player death");

        vector<CNameIndex> vtxPos;
        if (dbset.name ().ExistsName (name)
            && !dbset.name ().ReadName (name, vtxPos))
          return error ("ConnectInputsGameTx: failed to read from name DB");
        CNameIndex txPos2(txPos, pindexBlock->nHeight,
                          vchFromString (VALUE_DEAD));
        vtxPos.push_back (txPos2);
        if (!dbset.name ().WriteName (name, vtxPos))
          return error ("ConnectInputsGameTx: failed to write to name DB");
    }

    return true;
}

static bool
DisconnectInputsGameTx (DatabaseSet& dbset, const CTransaction& tx,
                        CBlockIndex* pindexBlock)
{
    if (!tx.IsGameTx())
        return error("DisconnectInputsGameTx called for non-game tx");

    for (int i = 0; i < tx.vin.size(); i++)
    {
        /* Skip bounty payouts.  */
        if (tx.vin[i].prevout.IsNull ())
          continue;

        vchType name;
        if (!IsPlayerDeathInput (tx.vin[i], name))
          return error ("DisconnectInputsGameTx: prev is no player death");

        vector<CNameIndex> vtxPos;
        if (dbset.name ().ExistsName (name)
            && !dbset.name ().ReadName (name, vtxPos))
          return error("DisconnectInputsGameTx() : failed to read from name DB");

        if (vtxPos.empty() || vtxPos.back().nHeight != pindexBlock->nHeight || vtxPos.back().vValue != vchFromString(VALUE_DEAD))
            printf("DisconnectInputsGameTx() : Warning: game transaction height mismatch (height %d, expected %d)\n", vtxPos.back().nHeight, pindexBlock->nHeight);
        while (!vtxPos.empty() && vtxPos.back().nHeight >= pindexBlock->nHeight)
            vtxPos.pop_back();

        if (!dbset.name ().WriteName (name, vtxPos))
            return error("DisconnectInputsGameTx() : failed to write to name DB");
    }

    return true;
}

bool
CHuntercoinHooks::ConnectInputs (DatabaseSet& dbset,
                                 const std::map<uint256, CTxIndex>& mapTestPool,
                                 const CTransaction& tx,
                                 const std::vector<CUtxoEntry>& vTxoPrev,
                                 const CBlockIndex* pindexBlock,
                                 const CDiskTxPos& txPos,
                                 bool fBlock, bool fMiner)
{
    if (tx.IsGameTx ())
      {
        if (fBlock)
          return ConnectInputsGameTx (dbset, mapTestPool, tx,
                                      pindexBlock, txPos);
        return true;
      }

    /* For game transactions, the vectors of previous transactions
       are not filled.  Check that they are filled properly if we have
       a non-game transaction.  */
    assert (vTxoPrev.size () == tx.vin.size ());

    int nInput;
    bool found = false;

    int prevHeight, prevOp;
    int64 prevCoinAmount = -1;
    std::vector<vchType> vvchPrevArgs;

    for (int i = 0 ; i < tx.vin.size(); i++)
    {
        const CTxOut& out = vTxoPrev[i].txo;

        int op;
        std::vector<vchType> vvchPrevArgsRead;
        if (DecodeNameScript(out.scriptPubKey, op, vvchPrevArgsRead))
        {
            if (found)
                return error("ConnectInputHook() : multiple previous name transactions");
            found = true;
            nInput = i;
            vvchPrevArgs = vvchPrevArgsRead;
            prevCoinAmount = out.nValue;
            prevHeight = vTxoPrev[i].height;
            prevOp = op;
        }
    }

    if (tx.nVersion != NAMECOIN_TX_VERSION)
    {
        // Make sure name-op outputs are not spent by a regular transaction, or the name
        // would be lost
        if (found)
            return error("ConnectInputHook() : a non-name transaction with a name input");
        return true;
    }

    std::vector<vchType> vvchArgs;
    int op, nOut;
    if (!DecodeNameTx(tx, op, nOut, vvchArgs))
        return error("ConnectInputsHook() : could not decode a name tx");

    /* Get depth of previous tx.  This is only meaningful if the
       prev tx is a name operation, otherwise prevHeight is still
       set to -1.  */
    const int nDepth = pindexBlock->nHeight - prevHeight;
    if (found && nDepth < 0)
      return error ("ConnectInputHook: depth negative"
                    " (block %d, prev %d, depth %d)",
                    pindexBlock->nHeight, prevHeight, nDepth);
    int64 nNetFee;

    switch (op)
    {
        case OP_NAME_NEW:
            if (found)
                return error("ConnectInputsHook() : name_new tx pointing to previous name tx");
            if (tx.vout[nOut].nValue < NAMENEW_COIN_AMOUNT)
                return error("ConnectInputsHook() : name_new tx: insufficient amount");
            break;

        case OP_NAME_FIRSTUPDATE:
            nNetFee = GetNameNetFee(tx);
            if (nNetFee < GetNetworkFee(pindexBlock->nHeight))
                return error("ConnectInputsHook() : got tx %s with fee too low %d", tx.GetHash().GetHex().c_str(), nNetFee);
            if (!found || prevOp != OP_NAME_NEW)
                return error("ConnectInputsHook() : name_firstupdate tx without previous name_new tx");

            {
                // Check hash
                const vchType& vchHash = vvchPrevArgs[0];
                const vchType& vchName = vvchArgs[0];
                const vchType& vchRand = vvchArgs[1];
                vchType vchToHash(vchRand);
                vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
                uint160 hash = Hash160(vchToHash);
                if (uint160(vchHash) != hash)
                    return error("ConnectInputsHook() : name_firstupdate hash mismatch");
            }

            if (tx.vout[nOut].nValue < GetNameCoinAmount (pindexBlock->nHeight))
                return error ("ConnectInputsHook: name_firstupdate tx:"
                              " insufficient amount of the locked coin");
            if (!NameAvailable (dbset, vvchArgs[0]))
                return error("ConnectInputsHook() : name_firstupdate on an existing name");

            /* Do not accept if name_new is not mature.  */
            if ((fBlock || fMiner) && nDepth < MIN_FIRSTUPDATE_DEPTH)
                return false;

            /* Check that no other pending txs on this name are already
               in the block to be mined.  */
            if (fMiner)
            {
                const set<uint256>& setPending = mapNamePending[vvchArgs[0]];
                BOOST_FOREACH(const PAIRTYPE(uint256, CTxIndex)& s, mapTestPool)
                {
                    if (setPending.count(s.first))
                    {
                        printf("ConnectInputsHook() : will not mine %s because it clashes with %s",
                                tx.GetHash().GetHex().c_str(),
                                s.first.GetHex().c_str());
                        return false;
                    }
                }
            }
            break;

        case OP_NAME_UPDATE:
            if (!found || (prevOp != OP_NAME_FIRSTUPDATE && prevOp != OP_NAME_UPDATE))
                return error("name_update tx without previous update tx");
            assert (prevCoinAmount >= 0);

            // Check name
            if (vvchPrevArgs[0] != vvchArgs[0])
                return error("ConnectInputsHook() : name_update name mismatch");

            /* Prevent update of a name twice in a single block.  */
            if ((fBlock || fMiner) && nDepth == 0)
              return error ("ConnectInputsHook: multiple name_update operations"
                            " on the same name");

            /* Check amount of locked coin.  */
            if (tx.vout[nOut].nValue != prevCoinAmount)
              return error ("ConnectInputsHook: name_update tx:"
                            " incorrect amount of the locked coin");
            break;

        default:
            return error("ConnectInputsHook() : name transaction has unknown op");
    }

    if (fBlock)
    {
        if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
        {
            vector<CNameIndex> vtxPos;
            if (dbset.name ().ExistsName (vvchArgs[0])
                && !dbset.name ().ReadName (vvchArgs[0], vtxPos))
              return error("ConnectInputsHook() : failed to read from name DB");
            vchType vchValue; // add
            int nHeight;
            uint256 hash;
            GetValueOfTxPos(txPos, vchValue, hash, nHeight);
            CNameIndex txPos2;
            txPos2.nHeight = pindexBlock->nHeight;
            txPos2.vValue = vchValue;
            txPos2.txPos = txPos;
            vtxPos.push_back(txPos2); // fin add
            //vtxPos.push_back(txPos);
            if (!dbset.name ().WriteName (vvchArgs[0], vtxPos))
                return error("ConnectInputsHook() : failed to write to name DB");

            CRITICAL_BLOCK(cs_main)
            {
                std::map<vchType, std::set<uint256> >::iterator mi;
                mi = mapNamePending.find (vvchArgs[0]);
                if (mi != mapNamePending.end())
                    mi->second.erase(tx.GetHash());
            }
        }
    }

    return true;
}

bool
CHuntercoinHooks::DisconnectInputs (DatabaseSet& dbset, const CTransaction& tx,
                                    CBlockIndex* pindexBlock)
{
    if (tx.IsGameTx ())
        return DisconnectInputsGameTx (dbset, tx, pindexBlock);

    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vchType> vvchArgs;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvchArgs);
    if (!good)
        return error("DisconnectInputsHook() : could not decode name tx");
    if (op == OP_NAME_FIRSTUPDATE || op == OP_NAME_UPDATE)
    {
        vector<CNameIndex> vtxPos;
        if (!dbset.name ().ReadName (vvchArgs[0], vtxPos))
            return error("DisconnectInputsHook() : failed to read from name DB");

        // vtxPos might be empty if we pruned expired transactions.  However, it should normally still not
        // be empty, since a reorg cannot go that far back.  Be safe anyway and do not try to pop if empty.
        if (vtxPos.empty() || vtxPos.back().nHeight != pindexBlock->nHeight)
            printf("DisconnectInputsHook() : Warning: name transaction height mismatch (height %d, expected %d)\n", vtxPos.back().nHeight, pindexBlock->nHeight);
        while (!vtxPos.empty() && vtxPos.back().nHeight >= pindexBlock->nHeight)
            vtxPos.pop_back();

        if (!dbset.name ().WriteName (vvchArgs[0], vtxPos))
            return error("DisconnectInputsHook() : failed to write to name DB");
    }

    return true;
}

bool CHuntercoinHooks::CheckTransaction(const CTransaction& tx)
{
    if (tx.nVersion != NAMECOIN_TX_VERSION)
        return true;

    vector<vector<unsigned char> > vvch;
    int op;
    int nOut;

    bool good = DecodeNameTx(tx, op, nOut, vvch);

    if (!good)
        return error("name transaction has unknown script format");

    Game::Move m;
    switch (op)
    {
        case OP_NAME_NEW:
            if (vvch[0].size() != 20)
                return error("name_new tx with incorrect hash length");
            break;
        case OP_NAME_FIRSTUPDATE:
            if (vvch[1].size() > 20)
                return error("name_firstupdate tx with rand too big");
            if (vvch[2].size() > MAX_VALUE_LENGTH)
                return error("name_firstupdate tx with value too long");
            m.Parse(stringFromVch(vvch[0]), stringFromVch(vvch[2]));
            if (!m)
                return error("name_firstupdate : incorrect game move");
            /* Move parsing already checks for valid player name, which
               in turn includes the length check.  */
            assert (vvch[0].size () <= MAX_NAME_LENGTH);
            break;
        case OP_NAME_UPDATE:
            if (vvch[1].size() > MAX_VALUE_LENGTH)
                return error("name_update tx with value too long");
            m.Parse(stringFromVch(vvch[0]), stringFromVch(vvch[1]));
            if (!m)
                return error("name_update : incorrect game move");
            /* Move parsing already checks for valid player name, which
               in turn includes the length check.  */
            assert (vvch[0].size () <= MAX_NAME_LENGTH);
            break;
        default:
            return error("name transaction has unknown op");
    }
    return true;
}

static string nameFromOp(int op)
{
    switch (op)
    {
        case OP_NAME_NEW:
            return "name_new";
        case OP_NAME_UPDATE:
            return "name_update";
        case OP_NAME_FIRSTUPDATE:
            return "name_firstupdate";
        default:
            return "<unknown name op>";
    }
}

bool CHuntercoinHooks::ExtractAddress(const CScript& script, string& address)
{
    if (script.size() == 1 && script[0] == OP_RETURN)
    {
        address = string("network fee");
        return true;
    }
    vector<vector<unsigned char> > vvch;
    int op;
    if (!DecodeNameScript(script, op, vvch))
        return false;

    string strOp = nameFromOp(op);
    string strName;
    if (op == OP_NAME_NEW)
    {
#ifdef GUI
        LOCK(cs_main);

        std::map<uint160, std::vector<unsigned char> >::const_iterator mi = mapMyNameHashes.find(uint160(vvch[0]));
        if (mi != mapMyNameHashes.end())
            strName = stringFromVch(mi->second);
        else
#endif
            strName = HexStr(vvch[0]);
    }
    else
        strName = stringFromVch(vvch[0]);

    address = strOp + ": " + strName;
    return true;
}

bool
CHuntercoinHooks::ConnectBlock (CBlock& block, DatabaseSet& dbset,
                                CBlockIndex* pindex, int64 &nFees,
                                unsigned int nPosAfterTx)
{
    if (!block.vgametx.empty())
    {
        block.vgametx.clear();
        block.vGameMerkleTree.clear();
        printf("ConnectBlock hook : non-empty vgametx, clearing and re-creating\n");
    }

    if (!AdvanceGameState (dbset, pindex, &block, nFees))
        return error("Connect block hook : AdvanceGameState failed");

    // If no game transactions or already written (e.g. if block was disconnected then reconnected),
    // skip tx generation and proceed to connecting them
    // The last check (hashGameMerkleRoot) can probably be omitted.
    if ((!block.vgametx.empty() && block.nGameTxFile == -1) || pindex->hashGameMerkleRoot != block.hashGameMerkleRoot)
    {
        block.hashGameMerkleRoot = block.BuildMerkleTree(true);
        if (pindex->hashGameMerkleRoot != block.hashGameMerkleRoot)
        {
            pindex->hashGameMerkleRoot = block.hashGameMerkleRoot;
            CDiskBlockIndex blockindex(pindex);
            if (!dbset.tx ().WriteBlockIndex (blockindex))
                return error("ConnectBlock hook : WriteBlockIndex failed");
        }

        // Write game transactions to disk. They are written outside of the block - just appended to the block file.
        CRITICAL_BLOCK(cs_AppendBlockFile)
        {
            char pchMessageVGameTx[8] = { 'v', 'g', 'a', 'm', 'e', 't', 'x', ':' };
            unsigned nSize = GetSerializeSize (block.vgametx, SER_DISK);
            nSize += sizeof (pchMessageVGameTx);

            unsigned nTotalSize = sizeof (pchMessageStart);
            nTotalSize += sizeof (nSize) + nSize;

            CAutoFile fileout = AppendBlockFile (dbset, block.nGameTxFile,
                                                 nTotalSize);
            if (!fileout)
                return error("ConnectBlock hook : AppendBlockFile failed");
            const unsigned startPos = ftell (fileout);
            fileout << FLATDATA(pchMessageStart) << nSize << FLATDATA(pchMessageVGameTx);

            block.nGameTxPos = ftell(fileout);
            if (block.nGameTxPos == -1)
                return error("ConnectBlock hook : ftell failed");
            fileout << block.vgametx;

            // Check that the total size estimate was correct.
            if (ftell (fileout) - startPos != nTotalSize)
              return error ("ConnectBlock hook: nTotalSize was wrong");

            FlushBlockFile(fileout);
        }

        // Update block fields that were changed (because they depend on the game transactions, which were just computed)
        {
            unsigned int nGameMerkleRootPos = pindex->nBlockPos + ::GetSerializeSize(block, SER_NETWORK | SER_BLOCKHEADERONLY);
            CAutoFile fileout = OpenBlockFile(pindex->nFile, nGameMerkleRootPos, "rb+");
            if (!fileout)
                return error("ConnectBlock hook : OpenBlockFile failed");

            fileout << block.hashGameMerkleRoot;

            if (fseek(fileout, nPosAfterTx, SEEK_SET) != 0)
                return error("ConnectBlock hook : fseek failed");

            fileout << block.nGameTxFile << block.nGameTxPos;

            FlushBlockFile(fileout);
        }
    }

    map<uint256, CTxIndex> mapUnused;
    unsigned int nTxPos = block.nGameTxPos + GetSizeOfCompactSize(block.vgametx.size());

    BOOST_FOREACH(CTransaction& tx, block.vgametx)
    {
        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, block.nGameTxFile, nTxPos);
        nTxPos += ::GetSerializeSize(tx, SER_DISK);

        if (!tx.ConnectInputs (dbset, mapUnused, posThisTx, pindex,
                               nFees, true, false))
            return error("ConnectBlock hook : ConnectInputs failed for game tx");
    }

    return true;
}

void
CHuntercoinHooks::NewBlockAdded ()
{
  cv_stateChange.notify_all ();
}

bool
CHuntercoinHooks::DisconnectBlock (CBlock& block, DatabaseSet& dbset,
                                   CBlockIndex* pindex)
{
  RollbackGameState (dbset.tx (), pindex);
  return true;
}

bool CHuntercoinHooks::GenesisBlock(CBlock& block)
{
    block = CBlock();
    block.hashPrevBlock = 0;
    block.nVersion = 1;
    block.nBits    = bnInitialHashTarget[0].GetCompact();
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vout[0].nValue = GetBlockValue(0, 0);
    if (fTestNet)
    {
        txNew.vin[0].scriptSig = CScript() << vchFromString("\nHuntercoin test net\n");
        txNew.vout[0].scriptPubKey.SetBitcoinAddress("hRDGZuirWznh25mqZM5bKmeEAcw7dmDwUx");
        txNew.vout[0].nValue = 100 * COIN;     // Preallocated coins for easy testing and giveaway
        block.nTime    = 1391193136;
        block.nNonce   = 1997599826u;
    }
    else
    {
        const char *timestamp =
                "\n"
                "Huntercoin genesis timestamp\n"
                "31/Jan/2014 20:10 GMT\n"
                "Bitcoin block 283440: 0000000000000001795d3c369b0746c0b5d315a6739a7410ada886de5d71ca86\n"
                "Litecoin block 506479: 77c49384e6e8dd322da0ebb32ca6c8f047d515d355e9f22b116430a888fffd38\n"
            ;
        txNew.vin[0].scriptSig = CScript() << vchFromString(std::string(timestamp));
        txNew.vout[0].scriptPubKey.SetBitcoinAddress("HVguPy1tWgbu9cKy6YGYEJFJ6RD7z7F7MJ");
        txNew.vout[0].nValue = 85000 * COIN;     // Preallocated coins for bounties and giveaway
        block.nTime    = 1391199780;
        block.nNonce   = 1906435634u;
    }
    block.vtx.push_back(txNew);
    block.hashMerkleRoot = block.BuildMerkleTree(false);

#if 0
    MineGenesisBlock(&block);
#endif

    printf("====================================\n");
    printf("Merkle: %s\n", block.hashMerkleRoot.GetHex().c_str());
    printf("Block: %s\n", block.GetHash().GetHex().c_str());
    block.print();
    assert(block.GetHash() == hashGenesisBlock);
    return true;
}

int CHuntercoinHooks::LockinHeight()
{
        return 0;
}

bool CHuntercoinHooks::Lockin(int nHeight, uint256 hash)
{
    return true;
}

string CHuntercoinHooks::IrcPrefix()
{
    return "huntercoin";
}

unsigned short GetDefaultPort()
{
    return fTestNet ? 18398 : 8398;
}

unsigned int pnSeed[] = { 0 };
const char *strDNSSeed[] = { NULL };

string GetDefaultDataDirSuffix() {
#ifdef __WXMSW__
    // Windows
    return string("Huntercoin");
#else
#ifdef MAC_OSX
    return string("Huntercoin");
#else
    return string(".huntercoin");
#endif
#endif
}

unsigned char GetAddressVersion() { return ((unsigned char)(fTestNet ? 100 : 40)); }
