// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/blockchain.h>

#include <amount.h>
#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <hash.h>
#include <index/blockfilterindex.h>
#include <index/coinstatsindex.h>
#include <node/blockstorage.h>
#include <node/coinstats.h>
#include <node/context.h>
#include <node/utxo_snapshot.h>
#include <key_io.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <primitives/transaction.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <streams.h>
#include <sync.h>
#include <txdb.h>
#include <txmempool.h>
#include <undo.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>
#include <warnings.h>
#include <libdevcore/CommonData.h>
#include <pow.h>
#include <pos.h>
#include <txdb.h>
#include <util/convert.h>
#include <ariel/qtumdelegation.h>
#include <util/tokenstr.h>
#include <rpc/contract_util.h>

#include <stdint.h>

#include <univalue.h>

#include <condition_variable>
#include <memory>
#include <mutex>

CTxMemPool& EnsureMemPool(const NodeContext& node)
{
    if (!node.mempool) {
        throw JSONRPCError(RPC_CLIENT_MEMPOOL_DISABLED, "Mempool disabled or instance not found");
    }
    return *node.mempool;
}

CTxMemPool& EnsureAnyMemPool(const std::any& context)
{
    return EnsureMemPool(EnsureAnyNodeContext(context));
}

CBlockPolicyEstimator& EnsureFeeEstimator(const NodeContext& node)
{
    if (!node.fee_estimator) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Fee estimation disabled");
    }
    return *node.fee_estimator;
}

CBlockPolicyEstimator& EnsureAnyFeeEstimator(const std::any& context)
{
    return EnsureFeeEstimator(EnsureAnyNodeContext(context));
}

/* Calculate the difficulty for a given block index.
 */
double GetDifficulty(const CBlockIndex* blockindex)
{
    CHECK_NONFATAL(blockindex);

    int nShift = (blockindex->nBits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

double GetPoWMHashPS(ChainstateManager& chainman)
{
    if (pindexBestHeader->nHeight >= Params().GetConsensus().nLastPOWBlock)
        return 0;

    int nPoWInterval = 72;
    int64_t nTargetSpacingWorkMin = 30, nTargetSpacingWork = 30;

    CChain& active_chain = chainman.ActiveChain();
    CBlockIndex* pindexGenesisBlock = active_chain.Genesis();
    CBlockIndex* pindex = pindexGenesisBlock;
    CBlockIndex* pindexPrevWork = pindexGenesisBlock;

    while (pindex)
    {
        if (pindex->IsProofOfWork())
        {
            int64_t nActualSpacingWork = pindex->GetBlockTime() - pindexPrevWork->GetBlockTime();
            nTargetSpacingWork = ((nPoWInterval - 1) * nTargetSpacingWork + nActualSpacingWork + nActualSpacingWork) / (nPoWInterval + 1);
            nTargetSpacingWork = std::max(nTargetSpacingWork, nTargetSpacingWorkMin);
            pindexPrevWork = pindex;
        }

        pindex = pindex->pnext;
    }

    return GetDifficulty(active_chain.Tip()) * 4294.967296 / nTargetSpacingWork;
}

double GetPoSKernelPS()
{
    int nPoSInterval = 72;
    double dStakeKernelsTriedAvg = 0;
    int nStakesHandled = 0, nStakesTime = 0;

    CBlockIndex* pindex = pindexBestHeader;
    CBlockIndex* pindexPrevStake = NULL;

    const Consensus::Params& consensusParams = Params().GetConsensus();
    bool dynamicStakeSpacing = true;
    uint32_t stakeTimestampMask=consensusParams.StakeTimestampMask(0);
    if(pindex)
    {
        dynamicStakeSpacing = pindex->nHeight < consensusParams.QIP9Height;
        stakeTimestampMask=consensusParams.StakeTimestampMask(pindex->nHeight);
    }

    while (pindex && nStakesHandled < nPoSInterval)
    {
        if (pindex->IsProofOfStake())
        {
            if (pindexPrevStake)
            {
                dStakeKernelsTriedAvg += GetDifficulty(pindexPrevStake) * 4294967296.0;
                if(dynamicStakeSpacing)
                    nStakesTime += pindexPrevStake->nTime - pindex->nTime;
                nStakesHandled++;
            }
            pindexPrevStake = pindex;
        }

        pindex = pindex->pprev;
    }

    if(!dynamicStakeSpacing)
    {
        // Using a fixed denominator reduces the variation spikes
        nStakesTime = consensusParams.TargetSpacing(pindexBestHeader->nHeight) * nStakesHandled;
    }

    double result = 0;

    if (nStakesTime)
        result = dStakeKernelsTriedAvg / nStakesTime;

    result *= stakeTimestampMask + 1;

    return result;
}

double GetEstimatedAnnualROI(ChainstateManager& chainman)
{
    double result = 0;
    double networkWeight = GetPoSKernelPS();
    CChain& active_chain = chainman.ActiveChain();
    CBlockIndex* pindex = pindexBestHeader == 0 ? active_chain.Tip() : pindexBestHeader;
    int nHeight = pindex ? pindex->nHeight : 0;
    const Consensus::Params& consensusParams = Params().GetConsensus();
    double subsidy = GetBlockSubsidy(nHeight, consensusParams);
    int nBlocktimeDownscaleFactor = consensusParams.BlocktimeDownscaleFactor(nHeight);
    if(networkWeight > 0)
    {
        // Formula: 100 * 675 blocks/day * 365 days * subsidy) / Network Weight
        result = nBlocktimeDownscaleFactor * 24637500 * subsidy / networkWeight;
    }

    return result;
}

static int ComputeNextBlockAndDepth(const CBlockIndex* tip, const CBlockIndex* blockindex, const CBlockIndex*& next)
{
    next = tip->GetAncestor(blockindex->nHeight + 1);
    if (next && next->pprev == blockindex) {
        return tip->nHeight - blockindex->nHeight + 1;
    }
    next = nullptr;
    return blockindex == tip ? 1 : -1;
}

CBlockIndex* ParseHashOrHeight(const UniValue& param, ChainstateManager& chainman) {
    LOCK(::cs_main);
    CChain& active_chain = chainman.ActiveChain();

    if (param.isNum()) {
        const int height{param.get_int()};
        if (height < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d is negative", height));
        }
        const int current_tip{active_chain.Height()};
        if (height > current_tip) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Target block height %d after current tip %d", height, current_tip));
        }

        return active_chain[height];
    } else {
        const uint256 hash{ParseHashV(param, "hash_or_height")};
        CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);

        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        return pindex;
    }
}

UniValue blockheaderToJSON(const CBlockIndex* tip, const CBlockIndex* blockindex)
{
    // Serialize passed information without accessing chain state of the active chain!
    AssertLockNotHeld(cs_main); // For performance reasons

    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", blockindex->GetBlockHash().GetHex());
    const CBlockIndex* pnext;
    int confirmations = ComputeNextBlockAndDepth(tip, blockindex, pnext);
    result.pushKV("confirmations", confirmations);
    result.pushKV("height", blockindex->nHeight);
    result.pushKV("version", blockindex->nVersion);
    result.pushKV("versionHex", strprintf("%08x", blockindex->nVersion));
    result.pushKV("merkleroot", blockindex->hashMerkleRoot.GetHex());
    result.pushKV("time", (int64_t)blockindex->nTime);
    result.pushKV("mediantime", (int64_t)blockindex->GetMedianTimePast());
    result.pushKV("nonce", (uint64_t)blockindex->nNonce);
    result.pushKV("bits", strprintf("%08x", blockindex->nBits));
    result.pushKV("difficulty", GetDifficulty(blockindex));
    result.pushKV("chainwork", blockindex->nChainWork.GetHex());
    result.pushKV("nTx", (uint64_t)blockindex->nTx);
    result.pushKV("hashStateRoot", blockindex->hashStateRoot.GetHex()); // ariel
    result.pushKV("hashUTXORoot", blockindex->hashUTXORoot.GetHex()); // ariel

    if(blockindex->IsProofOfStake()){
        result.pushKV("prevoutStakeHash", blockindex->prevoutStake.hash.GetHex()); // ariel
        result.pushKV("prevoutStakeVoutN", (int64_t)blockindex->prevoutStake.n); // ariel
    }

    if (blockindex->pprev)
        result.pushKV("previousblockhash", blockindex->pprev->GetBlockHash().GetHex());
    if (pnext)
        result.pushKV("nextblockhash", pnext->GetBlockHash().GetHex());

    result.pushKV("flags", strprintf("%s", blockindex->IsProofOfStake()? "proof-of-stake" : "proof-of-work"));
    result.pushKV("proofhash", blockindex->hashProof.GetHex());
    result.pushKV("modifier", blockindex->nStakeModifier.GetHex());

    if (blockindex->IsProofOfStake())
    {
        std::vector<unsigned char> vchBlockSig = blockindex->GetBlockSignature();
        result.pushKV("signature", HexStr(vchBlockSig));
        if(blockindex->HasProofOfDelegation())
        {
            std::vector<unsigned char> vchPoD = blockindex->GetProofOfDelegation();
            result.pushKV("proofOfDelegation", HexStr(vchPoD));
        }
    }

    return result;
}

UniValue blockToJSON(const CBlock& block, const CBlockIndex* tip, const CBlockIndex* blockindex, bool txDetails)
{
    UniValue result = blockheaderToJSON(tip, blockindex);

    result.pushKV("strippedsize", (int)::GetSerializeSize(block, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS));
    result.pushKV("size", (int)::GetSerializeSize(block, PROTOCOL_VERSION));
    result.pushKV("weight", (int)::GetBlockWeight(block));
    UniValue txs(UniValue::VARR);
    if (txDetails) {
        CBlockUndo blockUndo;
        const bool have_undo = !IsBlockPruned(blockindex) && UndoReadFromDisk(blockUndo, blockindex);
        for (size_t i = 0; i < block.vtx.size(); ++i) {
            const CTransactionRef& tx = block.vtx.at(i);
            // coinbase transaction (i == 0) doesn't have undo data
            const CTxUndo* txundo = (have_undo && i) ? &blockUndo.vtxundo.at(i - 1) : nullptr;
            UniValue objTx(UniValue::VOBJ);
            TxToUniv(*tx, uint256(), objTx, true, RPCSerializationFlags(), txundo);
            txs.push_back(objTx);
        }
    } else {
        for (const CTransactionRef& tx : block.vtx) {
            txs.push_back(tx->GetHash().GetHex());
        }
    }
    result.pushKV("tx", txs);

    return result;
}

static RPCHelpMan getestimatedannualroi()
{
    return RPCHelpMan{"getestimatedannualroi",
                "\nReturns the estimated annual roi.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "The current estimated annual roi"},
                RPCExamples{
                    HelpExampleCli("getestimatedannualroi", "")
            + HelpExampleRpc("getestimatedannualroi", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return GetEstimatedAnnualROI(chainman);
},
    };
}

static RPCHelpMan getblockcount()
{
    return RPCHelpMan{"getblockcount",
                "\nReturns the height of the most-work fully-validated chain.\n"
                "The genesis block has height 0.\n",
                {},
                RPCResult{
                    RPCResult::Type::NUM, "", "The current block count"},
                RPCExamples{
                    HelpExampleCli("getblockcount", "")
            + HelpExampleRpc("getblockcount", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Height();
},
    };
}

static RPCHelpMan getbestblockhash()
{
    return RPCHelpMan{"getbestblockhash",
                "\nReturns the hash of the best (tip) block in the most-work fully-validated chain.\n",
                {},
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "the block hash, hex-encoded"},
                RPCExamples{
                    HelpExampleCli("getbestblockhash", "")
            + HelpExampleRpc("getbestblockhash", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return chainman.ActiveChain().Tip()->GetBlockHash().GetHex();
},
    };
}

void RPCNotifyBlockChange(const CBlockIndex* pindex)
{
    if(pindex) {
        LOCK(cs_blockchange);
        latestblock.hash = pindex->GetBlockHash();
        latestblock.height = pindex->nHeight;
    }
    cond_blockchange.notify_all();
}

static RPCHelpMan waitfornewblock()
{
    return RPCHelpMan{"waitfornewblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitfornewblock", "1000")
            + HelpExampleRpc("waitfornewblock", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;
    if (!request.params[0].isNull())
        timeout = request.params[0].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        block = latestblock;
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        else
            cond_blockchange.wait(lock, [&block]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblock()
{
    return RPCHelpMan{"waitforblock",
                "\nWaits for a specific new block and returns useful info about it.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Block hash to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\" 1000")
            + HelpExampleRpc("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\", 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&hash]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.hash == hash || !IsRPCRunning(); });
        block = latestblock;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan waitforblockheight()
{
    return RPCHelpMan{"waitforblockheight",
                "\nWaits for (at least) block height and returns the height and hash\n"
                "of the current tip.\n"
                "\nReturns the current block on timeout or exit.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "Block height to wait for."},
                    {"timeout", RPCArg::Type::NUM, RPCArg::Default{0}, "Time in milliseconds to wait for a response. 0 indicates no timeout."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hash", "The blockhash"},
                        {RPCResult::Type::NUM, "height", "Block height"},
                    }},
                RPCExamples{
                    HelpExampleCli("waitforblockheight", "100 1000")
            + HelpExampleRpc("waitforblockheight", "100, 1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int timeout = 0;

    int height = request.params[0].get_int();

    if (!request.params[1].isNull())
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        WAIT_LOCK(cs_blockchange, lock);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&height]() EXCLUSIVE_LOCKS_REQUIRED(cs_blockchange) {return latestblock.height >= height || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
},
    };
}

static RPCHelpMan syncwithvalidationinterfacequeue()
{
    return RPCHelpMan{"syncwithvalidationinterfacequeue",
                "\nWaits for the validation interface queue to catch up on everything that was there when we entered this function.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("syncwithvalidationinterfacequeue","")
            + HelpExampleRpc("syncwithvalidationinterfacequeue","")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    SyncWithValidationInterfaceQueue();
    return NullUniValue;
},
    };
}

static RPCHelpMan getdifficulty()
{
    return RPCHelpMan{"getdifficulty",
                "\nReturns the proof-of-work difficulty as a multiple of the minimum difficulty.\n",
//                "\nReturns the proof-of-stake difficulty as a multiple of the minimum difficulty.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "proof-of-work", "the proof-of-work difficulty as a multiple of the minimum difficulty."}
//                        {RPCResult::Type::NUM, "proof-of-stake", "the proof-of-stake difficulty as a multiple of the minimum difficulty."},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getdifficulty", "")
            + HelpExampleRpc("getdifficulty", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    UniValue obj(UniValue::VOBJ);
    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    obj.pushKV("proof-of-work",        GetDifficulty(GetLastBlockIndex(tip, false)));
//    obj.pushKV("proof-of-stake",       GetDifficulty(GetLastBlockIndex(tip, true)));
    return obj;
},
    };
}

static std::vector<RPCResult> MempoolEntryDescription() { return {
    RPCResult{RPCResult::Type::NUM, "vsize", "virtual transaction size as defined in BIP 141. This is different from actual serialized size for witness transactions as witness data is discounted."},
    RPCResult{RPCResult::Type::NUM, "weight", "transaction weight as defined in BIP 141."},
    RPCResult{RPCResult::Type::STR_AMOUNT, "fee", "transaction fee in " + CURRENCY_UNIT + " (DEPRECATED)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "modifiedfee", "transaction fee with fee deltas used for mining priority (DEPRECATED)"},
    RPCResult{RPCResult::Type::NUM_TIME, "time", "local time transaction entered pool in seconds since 1 Jan 1970 GMT"},
    RPCResult{RPCResult::Type::NUM, "height", "block height when transaction entered pool"},
    RPCResult{RPCResult::Type::NUM, "descendantcount", "number of in-mempool descendant transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "descendantsize", "virtual transaction size of in-mempool descendants (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "descendantfees", "modified fees (see above) of in-mempool descendants (including this one) (DEPRECATED)"},
    RPCResult{RPCResult::Type::NUM, "ancestorcount", "number of in-mempool ancestor transactions (including this one)"},
    RPCResult{RPCResult::Type::NUM, "ancestorsize", "virtual transaction size of in-mempool ancestors (including this one)"},
    RPCResult{RPCResult::Type::STR_AMOUNT, "ancestorfees", "modified fees (see above) of in-mempool ancestors (including this one) (DEPRECATED)"},
    RPCResult{RPCResult::Type::STR_HEX, "wtxid", "hash of serialized transaction, including witness data"},
    RPCResult{RPCResult::Type::OBJ, "fees", "",
        {
            RPCResult{RPCResult::Type::STR_AMOUNT, "base", "transaction fee in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "modified", "transaction fee with fee deltas used for mining priority in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "ancestor", "modified fees (see above) of in-mempool ancestors (including this one) in " + CURRENCY_UNIT},
            RPCResult{RPCResult::Type::STR_AMOUNT, "descendant", "modified fees (see above) of in-mempool descendants (including this one) in " + CURRENCY_UNIT},
        }},
    RPCResult{RPCResult::Type::ARR, "depends", "unconfirmed transactions used as inputs for this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "parent transaction id"}}},
    RPCResult{RPCResult::Type::ARR, "spentby", "unconfirmed transactions spending outputs from this transaction",
        {RPCResult{RPCResult::Type::STR_HEX, "transactionid", "child transaction id"}}},
    RPCResult{RPCResult::Type::BOOL, "bip125-replaceable", "Whether this transaction could be replaced due to BIP125 (replace-by-fee)"},
    RPCResult{RPCResult::Type::BOOL, "unbroadcast", "Whether this transaction is currently unbroadcast (initial broadcast not yet acknowledged by any peers)"},
};}

static void entryToJSON(const CTxMemPool& pool, UniValue& info, const CTxMemPoolEntry& e) EXCLUSIVE_LOCKS_REQUIRED(pool.cs)
{
    AssertLockHeld(pool.cs);

    UniValue fees(UniValue::VOBJ);
    fees.pushKV("base", ValueFromAmount(e.GetFee()));
    fees.pushKV("modified", ValueFromAmount(e.GetModifiedFee()));
    fees.pushKV("ancestor", ValueFromAmount(e.GetModFeesWithAncestors()));
    fees.pushKV("descendant", ValueFromAmount(e.GetModFeesWithDescendants()));
    info.pushKV("fees", fees);

    info.pushKV("vsize", (int)e.GetTxSize());
    info.pushKV("weight", (int)e.GetTxWeight());
    info.pushKV("fee", ValueFromAmount(e.GetFee()));
    info.pushKV("modifiedfee", ValueFromAmount(e.GetModifiedFee()));
    info.pushKV("time", count_seconds(e.GetTime()));
    info.pushKV("height", (int)e.GetHeight());
    info.pushKV("descendantcount", e.GetCountWithDescendants());
    info.pushKV("descendantsize", e.GetSizeWithDescendants());
    info.pushKV("descendantfees", e.GetModFeesWithDescendants());
    info.pushKV("ancestorcount", e.GetCountWithAncestors());
    info.pushKV("ancestorsize", e.GetSizeWithAncestors());
    info.pushKV("ancestorfees", e.GetModFeesWithAncestors());
    info.pushKV("wtxid", pool.vTxHashes[e.vTxHashesIdx].first.ToString());
    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin)
    {
        if (pool.exists(txin.prevout.hash))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends)
    {
        depends.push_back(dep);
    }

    info.pushKV("depends", depends);

    UniValue spent(UniValue::VARR);
    const CTxMemPool::txiter& it = pool.mapTx.find(tx.GetHash());
    const CTxMemPoolEntry::Children& children = it->GetMemPoolChildrenConst();
    for (const CTxMemPoolEntry& child : children) {
        spent.push_back(child.GetTx().GetHash().ToString());
    }

    info.pushKV("spentby", spent);

    // Add opt-in RBF status
    bool rbfStatus = false;
    RBFTransactionState rbfState = IsRBFOptIn(tx, pool);
    if (rbfState == RBFTransactionState::UNKNOWN) {
        throw JSONRPCError(RPC_MISC_ERROR, "Transaction is not in mempool");
    } else if (rbfState == RBFTransactionState::REPLACEABLE_BIP125) {
        rbfStatus = true;
    }

    info.pushKV("bip125-replaceable", rbfStatus);
    info.pushKV("unbroadcast", pool.IsUnbroadcastTx(tx.GetHash()));
}

UniValue MempoolToJSON(const CTxMemPool& pool, bool verbose, bool include_mempool_sequence)
{
    if (verbose) {
        if (include_mempool_sequence) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Verbose results cannot contain mempool sequence values.");
        }
        LOCK(pool.cs);
        UniValue o(UniValue::VOBJ);
        for (const CTxMemPoolEntry& e : pool.mapTx) {
            const uint256& hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(pool, info, e);
            // Mempool has unique entries so there is no advantage in using
            // UniValue::pushKV, which checks if the key already exists in O(N).
            // UniValue::__pushKV is used instead which currently is O(1).
            o.__pushKV(hash.ToString(), info);
        }
        return o;
    } else {
        uint64_t mempool_sequence;
        std::vector<uint256> vtxid;
        {
            LOCK(pool.cs);
            pool.queryHashes(vtxid);
            mempool_sequence = pool.GetSequence();
        }
        UniValue a(UniValue::VARR);
        for (const uint256& hash : vtxid)
            a.push_back(hash.ToString());

        if (!include_mempool_sequence) {
            return a;
        } else {
            UniValue o(UniValue::VOBJ);
            o.pushKV("txids", a);
            o.pushKV("mempool_sequence", mempool_sequence);
            return o;
        }
    }
}

static RPCHelpMan getrawmempool()
{
    return RPCHelpMan{"getrawmempool",
                "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n"
                "\nHint: use getmempoolentry to fetch a specific transaction from the mempool.\n",
                {
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                    {"mempool_sequence", RPCArg::Type::BOOL, RPCArg::Default{false}, "If verbose=false, returns a json object with transaction list and mempool sequence number attached."},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "", "The transaction id"},
                        }},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                    RPCResult{"for verbose = false and mempool_sequence = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ARR, "txids", "",
                            {
                                {RPCResult::Type::STR_HEX, "", "The transaction id"},
                            }},
                            {RPCResult::Type::NUM, "mempool_sequence", "The mempool sequence value."},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getrawmempool", "true")
            + HelpExampleRpc("getrawmempool", "true")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[0].isNull())
        fVerbose = request.params[0].get_bool();

    bool include_mempool_sequence = false;
    if (!request.params[1].isNull()) {
        include_mempool_sequence = request.params[1].get_bool();
    }

    return MempoolToJSON(EnsureAnyMemPool(request.context), fVerbose, include_mempool_sequence);
},
    };
}

static RPCHelpMan getmempoolancestors()
{
    return RPCHelpMan{"getmempoolancestors",
                "\nIf txid is in the mempool, returns all in-mempool ancestors.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool ancestor transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempoolancestors", "\"mytxid\"")
            + HelpExampleRpc("getmempoolancestors", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setAncestors;
    uint64_t noLimit = std::numeric_limits<uint64_t>::max();
    std::string dummy;
    mempool.CalculateMemPoolAncestors(*it, setAncestors, noLimit, noLimit, noLimit, noLimit, dummy, false);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            o.push_back(ancestorIt->GetTx().GetHash().ToString());
        }
        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter ancestorIt : setAncestors) {
            const CTxMemPoolEntry &e = *ancestorIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempooldescendants()
{
    return RPCHelpMan{"getmempooldescendants",
                "\nIf txid is in the mempool, returns all in-mempool descendants.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "True for a json object, false for array of transaction ids"},
                },
                {
                    RPCResult{"for verbose = false",
                        RPCResult::Type::ARR, "", "",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id of an in-mempool descendant transaction"}}},
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ_DYN, "", "",
                        {
                            {RPCResult::Type::OBJ, "transactionid", "", MempoolEntryDescription()},
                        }},
                },
                RPCExamples{
                    HelpExampleCli("getmempooldescendants", "\"mytxid\"")
            + HelpExampleRpc("getmempooldescendants", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    bool fVerbose = false;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    CTxMemPool::setEntries setDescendants;
    mempool.CalculateDescendants(it, setDescendants);
    // CTxMemPool::CalculateDescendants will include the given tx
    setDescendants.erase(it);

    if (!fVerbose) {
        UniValue o(UniValue::VARR);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            o.push_back(descendantIt->GetTx().GetHash().ToString());
        }

        return o;
    } else {
        UniValue o(UniValue::VOBJ);
        for (CTxMemPool::txiter descendantIt : setDescendants) {
            const CTxMemPoolEntry &e = *descendantIt;
            const uint256& _hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(mempool, info, e);
            o.pushKV(_hash.ToString(), info);
        }
        return o;
    }
},
    };
}

static RPCHelpMan getmempoolentry()
{
    return RPCHelpMan{"getmempoolentry",
                "\nReturns mempool data for given transaction\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id (must be in mempool)"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "", MempoolEntryDescription()},
                RPCExamples{
                    HelpExampleCli("getmempoolentry", "\"mytxid\"")
            + HelpExampleRpc("getmempoolentry", "\"mytxid\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash = ParseHashV(request.params[0], "parameter 1");

    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    LOCK(mempool.cs);

    CTxMemPool::txiter it = mempool.mapTx.find(hash);
    if (it == mempool.mapTx.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
    }

    const CTxMemPoolEntry &e = *it;
    UniValue info(UniValue::VOBJ);
    entryToJSON(mempool, info, e);
    return info;
},
    };
}

static RPCHelpMan getblockhash()
{
    return RPCHelpMan{"getblockhash",
                "\nReturns hash of block in best-block-chain at height provided.\n",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The height index"},
                },
                RPCResult{
                    RPCResult::Type::STR_HEX, "", "The block hash"},
                RPCExamples{
                    HelpExampleCli("getblockhash", "1000")
            + HelpExampleRpc("getblockhash", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();

    int nHeight = request.params[0].get_int();
    if (nHeight < 0 || nHeight > active_chain.Height())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CBlockIndex* pblockindex = active_chain[nHeight];
    return pblockindex->GetBlockHash().GetHex();
},
    };
}

static RPCHelpMan getaccountinfo()
{
    return RPCHelpMan{"getaccountinfo",
                "\nGet contract details including balance, storage data and code.\n",
                {
                    {"address", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The address of the contract"},
                        {RPCResult::Type::STR_AMOUNT, "balance", "The balance of the contract"},
                        {RPCResult::Type::STR, "storage", "The storage data of the contract"},
                        {RPCResult::Type::STR_HEX, "code", "The bytecode of the contract"},
                    }},
                RPCExamples{
                    HelpExampleCli("getaccountinfo", "eb23c0b3e6042821da281a2e2364feb22dd543e3")
            + HelpExampleRpc("getaccountinfo", "eb23c0b3e6042821da281a2e2364feb22dd543e3")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    LOCK(cs_main);

    std::string strAddr = request.params[0].get_str();
    if(strAddr.size() != 40 || !CheckHex(strAddr))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect address");

    dev::Address addrAccount(strAddr);
    if(!globalState->addressInUse(addrAccount))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Address does not exist");

    UniValue result(UniValue::VOBJ);

    result.pushKV("address", strAddr);
    result.pushKV("balance", CAmount(globalState->balance(addrAccount)));
    std::vector<uint8_t> code(globalState->code(addrAccount));
    auto storage(globalState->storage(addrAccount));

    UniValue storageUV(UniValue::VOBJ);
    for (auto j: storage)
    {
        UniValue e(UniValue::VOBJ);
        e.pushKV(dev::toHex(dev::h256(j.second.first)), dev::toHex(dev::h256(j.second.second)));
        storageUV.pushKV(j.first.hex(), e);
    }

    result.pushKV("storage", storageUV);

    result.pushKV("code", HexStr(code));

    std::unordered_map<dev::Address, Vin> vins = globalState->vins();
    if(vins.count(addrAccount)){
        UniValue vin(UniValue::VOBJ);
        valtype vchHash(vins[addrAccount].hash.asBytes());
        std::reverse(vchHash.begin(), vchHash.end());
        vin.pushKV("hash", HexStr(vchHash));
        vin.pushKV("nVout", uint64_t(vins[addrAccount].nVout));
        vin.pushKV("value", uint64_t(vins[addrAccount].value));
        result.pushKV("vin", vin);
    }
    return result;
},
    };
}

static RPCHelpMan getstorage()
{
    return RPCHelpMan{"getstorage",
                "\nGet contract storage data.\n",
                {
                    {"address", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                    {"blocknum", RPCArg::Type::NUM,  RPCArg::Default{-1}, "Number of block to get state from."},
                    {"index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "Zero-based index position of the storage"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "The storage data of the contract",
                    {
                        {RPCResult::Type::OBJ, "", true, "",
                        {
                            {RPCResult::Type::STR_HEX, "", ""},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getstorage", "eb23c0b3e6042821da281a2e2364feb22dd543e3")
            + HelpExampleRpc("getstorage", "eb23c0b3e6042821da281a2e2364feb22dd543e3")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);

    CChain& active_chain = chainman.ActiveChain();
    std::string strAddr = request.params[0].get_str();
    if(strAddr.size() != 40 || !CheckHex(strAddr))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect address");

    TemporaryState ts(globalState);
    if (!request.params[1].isNull())
    {
        if (request.params[1].isNum())
        {
            auto blockNum = request.params[1].get_int();
            if((blockNum < 0 && blockNum != -1) || blockNum > active_chain.Height())
                throw JSONRPCError(RPC_INVALID_PARAMS, "Incorrect block number");

            if(blockNum != -1)
                ts.SetRoot(uintToh256(active_chain[blockNum]->hashStateRoot), uintToh256(active_chain[blockNum]->hashUTXORoot));

        } else {
            throw JSONRPCError(RPC_INVALID_PARAMS, "Incorrect block number");
        }
    }

    dev::Address addrAccount(strAddr);
    if(!globalState->addressInUse(addrAccount))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Address does not exist");

    UniValue result(UniValue::VOBJ);

    bool onlyIndex = !request.params[2].isNull();
    unsigned index = 0;
    if (onlyIndex)
        index = request.params[2].get_int();

    auto storage(globalState->storage(addrAccount));

    if (onlyIndex)
    {
        if (index >= storage.size())
        {
            std::ostringstream stringStream;
            stringStream << "Storage size: " << storage.size() << " got index: " << index;
            throw JSONRPCError(RPC_INVALID_PARAMS, stringStream.str());
        }
        auto elem = std::next(storage.begin(), index);
        UniValue e(UniValue::VOBJ);

        storage = {{elem->first, {elem->second.first, elem->second.second}}};
    }
    for (const auto& j: storage)
    {
        UniValue e(UniValue::VOBJ);
        e.pushKV(dev::toHex(dev::h256(j.second.first)), dev::toHex(dev::h256(j.second.second)));
        result.pushKV(j.first.hex(), e);
    }
    return result;
},
    };
}

static RPCHelpMan getblockheader()
{
    return RPCHelpMan{"getblockheader",
                "\nIf verbose is false, returns a string that is serialized, hex-encoded data for blockheader 'hash'.\n"
                "If verbose is true, returns an Object with information about blockheader <hash>.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{true}, "true for a json object, false for the hex-encoded data"},
                },
                {
                    RPCResult{"for verbose = true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                            {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                            {RPCResult::Type::NUM, "height", "The block height or index"},
                            {RPCResult::Type::NUM, "version", "The block version"},
                            {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                            {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                            {RPCResult::Type::NUM_TIME, "time", "The block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                            {RPCResult::Type::NUM, "nonce", "The nonce"},
                            {RPCResult::Type::STR_HEX, "bits", "The bits"},
                            {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                            {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the current chain"},
                            {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                            {RPCResult::Type::STR_HEX, "previousblockhash", /* optional */ true, "The hash of the previous block (if available)"},
                            {RPCResult::Type::STR_HEX, "nextblockhash", /* optional */ true, "The hash of the next block (if available)"},
                        }},
                    RPCResult{"for verbose=false",
                        RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                },
                RPCExamples{
                    HelpExampleCli("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "hash"));

    bool fVerbose = true;
    if (!request.params[1].isNull())
        fVerbose = request.params[1].get_bool();

    const CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();
    }

    if (!pblockindex) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
    }

    if (!fVerbose)
    {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << pblockindex->GetBlockHeader();
        std::string strHex = HexStr(ssBlock);
        return strHex;
    }

    return blockheaderToJSON(tip, pblockindex);
},
    };
}

static CBlock GetBlockChecked(const CBlockIndex* pblockindex)
{
    CBlock block;
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Block not available (pruned data)");
    }

    if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus())) {
        // Block not found on disk. This could be because we have the block
        // header in our index but not yet have the block or did not accept the
        // block.
        throw JSONRPCError(RPC_MISC_ERROR, "Block not found on disk");
    }

    return block;
}

static CBlockUndo GetUndoChecked(const CBlockIndex* pblockindex)
{
    CBlockUndo blockUndo;
    if (IsBlockPruned(pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Undo data not available (pruned data)");
    }

    if (!UndoReadFromDisk(blockUndo, pblockindex)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Can't read undo data from disk");
    }

    return blockUndo;
}

static RPCHelpMan getblock()
{
    return RPCHelpMan{"getblock",
                "\nIf verbosity is 0, returns a string that is serialized, hex-encoded data for block 'hash'.\n"
                "If verbosity is 1, returns an Object with information about block <hash>.\n"
                "If verbosity is 2, returns an Object with information about block <hash> and information about each transaction. \n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The block hash"},
                    {"verbosity|verbose", RPCArg::Type::NUM, RPCArg::Default{1}, "0 for hex-encoded data, 1 for a json object, and 2 for json object with transaction data"},
                },
                {
                    RPCResult{"for verbosity = 0",
                RPCResult::Type::STR_HEX, "", "A string that is serialized, hex-encoded data for block 'hash'"},
                    RPCResult{"for verbosity = 1",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::STR_HEX, "hash", "the block hash (same as provided)"},
                    {RPCResult::Type::NUM, "confirmations", "The number of confirmations, or -1 if the block is not on the main chain"},
                    {RPCResult::Type::NUM, "size", "The block size"},
                    {RPCResult::Type::NUM, "strippedsize", "The block size excluding witness data"},
                    {RPCResult::Type::NUM, "weight", "The block weight as defined in BIP 141"},
                    {RPCResult::Type::NUM, "height", "The block height or index"},
                    {RPCResult::Type::NUM, "version", "The block version"},
                    {RPCResult::Type::STR_HEX, "versionHex", "The block version formatted in hexadecimal"},
                    {RPCResult::Type::STR_HEX, "merkleroot", "The merkle root"},
                    {RPCResult::Type::ARR, "tx", "The transaction ids",
                        {{RPCResult::Type::STR_HEX, "", "The transaction id"}}},
                    {RPCResult::Type::NUM_TIME, "time",       "The block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM_TIME, "mediantime", "The median block time expressed in " + UNIX_EPOCH_TIME},
                    {RPCResult::Type::NUM, "nonce", "The nonce"},
                    {RPCResult::Type::STR_HEX, "bits", "The bits"},
                    {RPCResult::Type::NUM, "difficulty", "The difficulty"},
                    {RPCResult::Type::STR_HEX, "chainwork", "Expected number of hashes required to produce the chain up to this block (in hex)"},
                    {RPCResult::Type::NUM, "nTx", "The number of transactions in the block"},
                    {RPCResult::Type::STR_HEX, "previousblockhash", /* optional */ true, "The hash of the previous block (if available)"},
                    {RPCResult::Type::STR_HEX, "nextblockhash", /* optional */ true, "The hash of the next block (if available)"},
                }},
                    RPCResult{"for verbosity = 2",
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::ELISION, "", "Same output as verbosity = 1"},
                    {RPCResult::Type::ARR, "tx", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::ELISION, "", "The transactions in the format of the getrawtransaction RPC. Different from verbosity = 1 \"tx\" result"},
                            {RPCResult::Type::NUM, "fee", "The transaction fee in " + CURRENCY_UNIT + ", omitted if block undo data is not available"},
                        }},
                    }},
                }},
        },
                RPCExamples{
                    HelpExampleCli("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    int verbosity = 1;
    if (!request.params[1].isNull()) {
        if (request.params[1].isBool()) {
            verbosity = request.params[1].get_bool() ? 1 : 0;
        } else {
            verbosity = request.params[1].get_int();
        }
    }

    CBlock block;
    const CBlockIndex* pblockindex;
    const CBlockIndex* tip;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        tip = chainman.ActiveChain().Tip();

        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        block = GetBlockChecked(pblockindex);
    }

    if (verbosity <= 0)
    {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION | RPCSerializationFlags());
        ssBlock << block;
        std::string strHex = HexStr(ssBlock);
        return strHex;
    }

    return blockToJSON(block, tip, pblockindex, verbosity >= 2);
},
    };
}

////////////////////////////////////////////////////////////////////// // ariel
RPCHelpMan callcontract()
{
    return RPCHelpMan{"callcontract",
                "\nCall contract methods offline, or test contract deployment offline.\n",
                {
                    {"address", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address, or empty address \"\""},
                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The data hex string"},
                    {"senderaddress", RPCArg::Type::STR, RPCArg::Optional::OMITTED_NAMED_ARG, "The sender address string"},
                    {"gaslimit", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The gas limit for executing the contract."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED_NAMED_ARG, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1, default: 0"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "address", "The address of the contract"},
                        {RPCResult::Type::OBJ, "executionResult", "The method execution result",
                            {
                                {RPCResult::Type::NUM, "gasUsed", "The gas used"},
                                {RPCResult::Type::NUM, "excepted", "The thrown exception"},
                                {RPCResult::Type::STR, "newAddress", "The new address of the contract"},
                                {RPCResult::Type::STR_HEX, "output", "The returned data from the method"},
                                {RPCResult::Type::NUM, "codeDeposit", "The code deposit"},
                                {RPCResult::Type::NUM, "gasRefunded", "The gas refunded"},
                                {RPCResult::Type::NUM, "depositSize", "The deposit size"},
                                {RPCResult::Type::NUM, "gasForDeposit", "The gas for deposit"},
                            }},
                        {RPCResult::Type::OBJ, "transactionReceipt", "The transaction receipt",
                            {
                                {RPCResult::Type::STR_HEX, "stateRoot", "The state root hash"},
                                {RPCResult::Type::NUM, "gasUsed", "The gas used"},
                                {RPCResult::Type::STR, "bloom", "The bloom"},
                                {RPCResult::Type::ARR, "log", "The logs from the receipt",
                                    {
                                        {RPCResult::Type::STR, "address", "The contract address"},
                                        {RPCResult::Type::ARR, "topics", "The topic",
                                            {{RPCResult::Type::STR_HEX, "topic", "The topic"}}},
                                        {RPCResult::Type::STR_HEX, "data", "The logged data"},
                                    }},

                            }},
                    }},
                RPCExamples{
                    HelpExampleCli("callcontract", "eb23c0b3e6042821da281a2e2364feb22dd543e3 06fdde03")
            + HelpExampleCli("callcontract", "\"\" 60606040525b33600060006101000a81548173ffffffffffffffffffffffffffffffffffffffff02191690836c010000000000000000000000009081020402179055506103786001600050819055505b600c80605b6000396000f360606040526008565b600256")
            + HelpExampleRpc("callcontract", "eb23c0b3e6042821da281a2e2364feb22dd543e3 06fdde03")
            + HelpExampleRpc("callcontract", "\"\" 60606040525b33600060006101000a81548173ffffffffffffffffffffffffffffffffffffffff02191690836c010000000000000000000000009081020402179055506103786001600050819055505b600c80605b6000396000f360606040526008565b600256")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    return CallToContract(request.params, chainman);
},
    };
}

class WaitForLogsParams {
public:
    int fromBlock;
    int toBlock;

    int minconf;

    std::set<dev::h160> addresses;
    std::vector<boost::optional<dev::h256>> topics;

    // bool wait;

    WaitForLogsParams(const UniValue& params) {
        std::unique_lock<std::mutex> lock(cs_blockchange);

        fromBlock = parseBlockHeight(params[0], latestblock.height + 1);
        toBlock = parseBlockHeight(params[1], -1);

        parseFilter(params[2]);
        minconf = parseUInt(params[3], 6);
    }

private:
    void parseFilter(const UniValue& val) {
        if (val.isNull()) {
            return;
        }

        parseParam(val["addresses"], addresses);
        parseParam(val["topics"], topics);
    }
};

RPCHelpMan waitforlogs()
{
    return RPCHelpMan{"waitforlogs",
                "requires -logevents to be enabled\n"
                "\nWaits for a new logs and return matching log entries. When the call returns, it also specifies the next block number to start waiting for new logs.\n"
                "By calling waitforlogs repeatedly using the returned `nextBlock` number, a client can receive a stream of up-to-date log entires.\n"
                "\nThis call is different from the similarly named `searchlogs`. This call returns individual matching log entries, `searchlogs` returns a transaction receipt if one of the log entries of that transaction matches the filter conditions.\n",
                {
                    {"fromblock", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The block number to start looking for logs."},
                    {"toblock", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The block number to stop looking for logs. If null, will wait indefinitely into the future."},
                    {"filter", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Filter conditions for logs.",
                    {
                        {"addresses", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "An address or a list of addresses to only get logs from particular account(s).",
                            {
                                {"address", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                            },
                        },
                        {"topics", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "An array of values from which at least one must appear in the log entries. The order is important, if you want to leave topics out use null, e.g. [null, \"0x00...\"].",
                            {
                                {"topic", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                            },
                        },
                    }},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{6}, "Minimal number of confirmations before a log is returned"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "entries", "Array of matchiing log entries. This may be empty if `filter` removed all entries.",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR_HEX, "blockHash", "The block hash"},
                                        {RPCResult::Type::NUM, "blockNumber", "The block number"},
                                        {RPCResult::Type::STR_HEX, "transactionHash", "The transaction hash"},
                                        {RPCResult::Type::NUM, "transactionIndex", "The transaction index"},
                                        {RPCResult::Type::STR, "from", "The from address"},
                                        {RPCResult::Type::STR, "to", "The to address"},
                                        {RPCResult::Type::NUM, "cumulativeGasUsed", "The cumulative gas used"},
                                        {RPCResult::Type::NUM, "gasUsed", "The gas used"},
                                        {RPCResult::Type::STR_HEX, "contractAddress", "The contract address"},
                                        {RPCResult::Type::STR, "excepted", "The thrown exception"},
                                        {RPCResult::Type::ARR, "topics", "The topic",
                                            {{RPCResult::Type::STR_HEX, "topic", "The topic"}}},
                                        {RPCResult::Type::STR_HEX, "data", "The logged data"},
                                    }
                                }
                            }
                        },
                        {RPCResult::Type::NUM, "count", "How many log entries are returned"},
                        {RPCResult::Type::NUM, "nextBlock", "To wait for new log entries haven't seen before, use this number as `fromBlock`"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("waitforlogs", "") + HelpExampleCli("waitforlogs", "600") + HelpExampleCli("waitforlogs", "600 700") + HelpExampleCli("waitforlogs", "null null")
                    + HelpExampleCli("waitforlogs", "null null '{ \"addresses\": [ \"12ae42729af478ca92c8c66773a3e32115717be4\" ], \"topics\": [ \"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"] }'")
            + HelpExampleRpc("waitforlogs", "") + HelpExampleRpc("waitforlogs", "600") + HelpExampleRpc("waitforlogs", "600 700") + HelpExampleRpc("waitforlogs", "null null")
            + HelpExampleRpc("waitforlogs", "null null '{ \"addresses\": [ \"12ae42729af478ca92c8c66773a3e32115717be4\" ], \"topics\": [ \"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"] }'")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request_) -> UniValue
{

    if (!fLogEvents)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Events indexing disabled");

    // this is a long poll function. force cast to non const pointer
    JSONRPCRequest& request = (JSONRPCRequest&) request_;
    if(!request.httpreq)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "HTTP connection not available");

    ChainstateManager& chainman = EnsureAnyChainman(request.context);

    WaitForLogsParams params(request.params);

    request.PollStart();

    std::vector<std::vector<uint256>> hashesToBlock;

    int curheight = 0;

    auto& addresses = params.addresses;
    auto& filterTopics = params.topics;

    while (curheight == 0) {
        {
            LOCK(cs_main);
            curheight = pblocktree->ReadHeightIndex(params.fromBlock, params.toBlock, params.minconf,
                    hashesToBlock, addresses, chainman);
        }

        // if curheight >= fromBlock. Blockchain extended with new log entries. Return next block height to client.
        //    nextBlock = curheight + 1
        // if curheight == 0. No log entry found in index. Wait for new block then try again.
        //    nextBlock = fromBlock
        // if curheight == -1. Incorrect parameters has entered.
        //
        // if curheight advanced, but all filtered out, API should return empty array, but advancing the cursor anyway.

        if (curheight > 0) {
            break;
        }

        if (curheight == -1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Incorrect params");
        }

        // wait for a new block to arrive
        {
            while (true) {
                std::unique_lock<std::mutex> lock(cs_blockchange);
                auto blockHeight = latestblock.height;

                request.PollPing();

                cond_blockchange.wait_for(lock, std::chrono::milliseconds(1000));
                if (latestblock.height > blockHeight) {
                    break;
                }

                // TODO: maybe just merge `IsRPCRunning` this into PollAlive
                if (!request.PollAlive() || !IsRPCRunning()) {
                    LogPrintf("waitforlogs client disconnected\n");
                    return NullUniValue;
                }
            }
        }
    }

    LOCK(cs_main);

    UniValue jsonLogs(UniValue::VARR);

    std::set<uint256> dupes;

    for (const auto& txHashes : hashesToBlock) {
        for (const auto& txHash : txHashes) {

            if(dupes.find(txHash) != dupes.end()) {
                continue;
            }
            dupes.insert(txHash);

            std::vector<TransactionReceiptInfo> receipts = pstorageresult->getResult(
                    uintToh256(txHash));

            for (const auto& receipt : receipts) {
                for (const auto& log : receipt.logs) {

                    bool includeLog = true;

                    if (!filterTopics.empty()) {
                        for (size_t i = 0; i < filterTopics.size(); i++) {
                            auto filterTopic = filterTopics[i];

                            if (!filterTopic) {
                                continue;
                            }

                            auto filterTopicContent = filterTopic.get();
                            auto topicContent = log.topics[i];

                            if (topicContent != filterTopicContent) {
                                includeLog = false;
                                break;
                            }
                        }
                    }


                    if (!includeLog) {
                        continue;
                    }

                    UniValue jsonLog(UniValue::VOBJ);

                    assignJSON(jsonLog, receipt);
                    assignJSON(jsonLog, log, false);

                    jsonLogs.push_back(jsonLog);
                }
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("entries", jsonLogs);
    result.pushKV("count", (int) jsonLogs.size());
    result.pushKV("nextblock", curheight + 1);

    return result;
},
    };
}

RPCHelpMan searchlogs()
{
    return RPCHelpMan{"searchlogs",
                "\nSearch logs, requires -logevents to be enabled.\n",
                {
                    {"fromblock", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of the earliest block (latest may be given to mean the most recent block)."},
                    {"toblock", RPCArg::Type::NUM, RPCArg::Optional::NO, "The number of the latest block (-1 may be given to mean the most recent block)."},
                    {"addressfilter", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Addresses filter conditions for logs.",
                    {
                        {"addresses", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "An address or a list of addresses to only get logs from particular account(s).",
                            {
                                {"address", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                            },
                        },
                    }},
                    {"topicfilter", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "Topics filter conditions for logs.",
                    {
                        {"topics", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "An array of values from which at least one must appear in the log entries. The order is important, if you want to leave topics out use null, e.g. [null, \"0x00...\"].",
                            {
                                {"topic", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                            },
                        },
                    }},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "Minimal number of confirmations before a log is returned"},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {
                        {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "blockHash", "The block hash"},
                                {RPCResult::Type::NUM, "blockNumber", "The block number"},
                                {RPCResult::Type::STR_HEX, "transactionHash", "The transaction hash"},
                                {RPCResult::Type::NUM, "transactionIndex", "The transaction index"},
                                {RPCResult::Type::STR, "from", "The from address"},
                                {RPCResult::Type::STR, "to", "The to address"},
                                {RPCResult::Type::NUM, "cumulativeGasUsed", "The cumulative gas used"},
                                {RPCResult::Type::NUM, "gasUsed", "The gas used"},
                                {RPCResult::Type::STR_HEX, "contractAddress", "The contract address"},
                                {RPCResult::Type::STR, "excepted", "The thrown exception"},
                                {RPCResult::Type::ARR, "log", "The logs from the receipt",
                                    {
                                        {RPCResult::Type::STR, "address", "The contract address"},
                                        {RPCResult::Type::ARR, "topics", "The topic",
                                            {{RPCResult::Type::STR_HEX, "topic", "The topic"}}},
                                        {RPCResult::Type::STR_HEX, "data", "The logged data"},
                                    }
                                },
                            }
                        }
                    }
                },
                RPCExamples{
                    HelpExampleCli("searchlogs", "0 100 '{\"addresses\": [\"12ae42729af478ca92c8c66773a3e32115717be4\"]}' '{\"topics\": [null,\"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"]}'")
            + HelpExampleRpc("searchlogs", "0 100 '{\"addresses\": [\"12ae42729af478ca92c8c66773a3e32115717be4\"]} {\"topics\": [null,\"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"]}'")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    return SearchLogs(request.params, chainman);
},
    };
}

RPCHelpMan gettransactionreceipt()
{
    return RPCHelpMan{"gettransactionreceipt",
                "\nGet the transaction receipt.\n",
                {
                    {"hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction hash"},
                },
               RPCResult{
            RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "blockHash", "The block hash"},
                            {RPCResult::Type::NUM, "blockNumber", "The block number"},
                            {RPCResult::Type::STR_HEX, "transactionHash", "The transaction hash"},
                            {RPCResult::Type::NUM, "transactionIndex", "The transaction index"},
                            {RPCResult::Type::STR, "from", "The from address"},
                            {RPCResult::Type::STR, "to", "The to address"},
                            {RPCResult::Type::NUM, "cumulativeGasUsed", "The cumulative gas used"},
                            {RPCResult::Type::NUM, "gasUsed", "The gas used"},
                            {RPCResult::Type::STR_HEX, "contractAddress", "The contract address"},
                            {RPCResult::Type::STR, "excepted", "The thrown exception"},
                            {RPCResult::Type::STR_HEX, "bloom", "Bloom filter for light clients to quickly retrieve related logs"},
                            {RPCResult::Type::ARR, "log", "The logs from the receipt",
                                {
                                    {RPCResult::Type::STR, "address", "The contract address"},
                                    {RPCResult::Type::ARR, "topics", "The topic",
                                        {{RPCResult::Type::STR_HEX, "topic", "The topic"}}},
                                    {RPCResult::Type::STR_HEX, "data", "The logged data"},
                                }},
                        }}
                }},
                RPCExamples{
                    HelpExampleCli("gettransactionreceipt", "3b04bc73afbbcf02cfef2ca1127b60fb0baf5f8946a42df67f1659671a2ec53c")
            + HelpExampleRpc("gettransactionreceipt", "3b04bc73afbbcf02cfef2ca1127b60fb0baf5f8946a42df67f1659671a2ec53c")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    if(!fLogEvents)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Events indexing disabled");

    LOCK(cs_main);

    std::string hashTemp = request.params[0].get_str();
    if(hashTemp.size() != 64){
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect hash");
    }

    uint256 hash(uint256S(hashTemp));

    std::vector<TransactionReceiptInfo> transactionReceiptInfo = pstorageresult->getResult(uintToh256(hash));

    UniValue result(UniValue::VARR);
    for(TransactionReceiptInfo& t : transactionReceiptInfo){
        UniValue tri(UniValue::VOBJ);
        transactionReceiptInfoToJSON(t, tri);
        result.push_back(tri);
    }
    return result;
},
    };
}

class DelegationsStakerFilter : public IDelegationFilter
{
public:
    DelegationsStakerFilter(const uint160& _address):
        address(_address)
    {}

    bool Match(const DelegationEvent& event) const override
    {
        return event.item.staker == address;
    }

private:
    uint160 address;
};

uint64_t getDelegateWeight(const uint160& keyid, const std::map<COutPoint, uint32_t>& immatureStakes, int height)
{
    // Decode address
    uint256 hashBytes;
    int type = 0;
    if (!DecodeIndexKey(EncodeDestination(PKHash(keyid)), hashBytes, type)) {
        return 0;
    }

    // Get address weight
    uint64_t weight = 0;
    if (!GetAddressWeight(hashBytes, type, immatureStakes, height, weight)) {
        return 0;
    }

    return weight;
}

//////////////////////////////////////////////////////////////////////

RPCHelpMan listcontracts()
{
    return RPCHelpMan{"listcontracts",
                "\nGet the contracts list.\n",
                {
                    {"start", RPCArg::Type::NUM, RPCArg::Default{1}, "The starting account index"},
                    {"maxdisplay", RPCArg::Type::NUM, RPCArg::Default{20}, "Max accounts to list"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "account", "The balance for the account"},
                    }},
                RPCExamples{
                    HelpExampleCli("listcontracts", "")
            + HelpExampleRpc("listcontracts", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

	LOCK(cs_main);

	int start=1;
	if (!request.params[0].isNull()){
		start = request.params[0].get_int();
		if (start<= 0)
			throw JSONRPCError(RPC_TYPE_ERROR, "Invalid start, min=1");
	}

	int maxDisplay=20;
	if (!request.params[1].isNull()){
		maxDisplay = request.params[1].get_int();
		if (maxDisplay <= 0)
			throw JSONRPCError(RPC_TYPE_ERROR, "Invalid maxDisplay");
	}

	UniValue result(UniValue::VOBJ);

	auto map = globalState->addresses();
	int contractsCount=(int)map.size();

	if (contractsCount>0 && start > contractsCount)
		throw JSONRPCError(RPC_TYPE_ERROR, "start greater than max index "+ i64tostr(contractsCount));

	int itStartPos=std::min(start-1,contractsCount);
	int i=0;
	for (auto it = std::next(map.begin(),itStartPos); it!=map.end(); it++)
	{
		result.pushKV(it->first.hex(),ValueFromAmount(CAmount(globalState->balance(it->first))));
		i++;
		if(i==maxDisplay)break;
	}

	return result;
},
    };
}

static RPCHelpMan pruneblockchain()
{
    return RPCHelpMan{"pruneblockchain", "",
                {
                    {"height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block height to prune up to. May be set to a discrete height, or to a " + UNIX_EPOCH_TIME + "\n"
            "                  to prune blocks whose block time is at least 2 hours older than the provided timestamp."},
                },
                RPCResult{
                    RPCResult::Type::NUM, "", "Height of the last block pruned"},
                RPCExamples{
                    HelpExampleCli("pruneblockchain", "1000")
            + HelpExampleRpc("pruneblockchain", "1000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (!fPruneMode)
        throw JSONRPCError(RPC_MISC_ERROR, "Cannot prune blocks because node is not in prune mode.");

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    int heightParam = request.params[0].get_int();
    if (heightParam < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative block height.");

    // Height value more than a billion is too high to be a block height, and
    // too low to be a block time (corresponds to timestamp from Sep 2001).
    if (heightParam > 1000000000) {
        // Add a 2 hour buffer to include blocks which might have had old timestamps
        CBlockIndex* pindex = active_chain.FindEarliestAtLeast(heightParam - TIMESTAMP_WINDOW, 0);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Could not find block with at least the specified timestamp.");
        }
        heightParam = pindex->nHeight;
    }

    unsigned int height = (unsigned int) heightParam;
    unsigned int chainHeight = (unsigned int) active_chain.Height();
    if (chainHeight < Params().PruneAfterHeight())
        throw JSONRPCError(RPC_MISC_ERROR, "Blockchain is too short for pruning.");
    else if (height > chainHeight)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Blockchain is shorter than the attempted prune height.");
    else if (height > chainHeight - MIN_BLOCKS_TO_KEEP) {
        LogPrint(BCLog::RPC, "Attempt to prune blocks close to the tip.  Retaining the minimum number of blocks.\n");
        height = chainHeight - MIN_BLOCKS_TO_KEEP;
    }

    PruneBlockFilesManual(active_chainstate, height);
    const CBlockIndex* block = active_chain.Tip();
    CHECK_NONFATAL(block);
    while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
        block = block->pprev;
    }
    return uint64_t(block->nHeight);
},
    };
}

CoinStatsHashType ParseHashType(const std::string& hash_type_input)
{
    if (hash_type_input == "hash_serialized_2") {
        return CoinStatsHashType::HASH_SERIALIZED;
    } else if (hash_type_input == "muhash") {
        return CoinStatsHashType::MUHASH;
    } else if (hash_type_input == "none") {
        return CoinStatsHashType::NONE;
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not a valid hash_type", hash_type_input));
    }
}

static RPCHelpMan gettxoutsetinfo()
{
    return RPCHelpMan{"gettxoutsetinfo",
                "\nReturns statistics about the unspent transaction output set.\n"
                "Note this call may take some time if you are not using coinstatsindex.\n",
                {
                    {"hash_type", RPCArg::Type::STR, RPCArg::Default{"hash_serialized_2"}, "Which UTXO set hash should be calculated. Options: 'hash_serialized_2' (the legacy algorithm), 'muhash', 'none'."},
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "The block hash or height of the target height (only available with coinstatsindex).", "", {"", "string or numeric"}},
                    {"use_index", RPCArg::Type::BOOL, RPCArg::Default{true}, "Use coinstatsindex, if available."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "height", "The block height (index) of the returned statistics"},
                        {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at which these statistics are calculated"},
                        {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs"},
                        {RPCResult::Type::NUM, "bogosize", "Database-independent, meaningless metric indicating the UTXO set size"},
                        {RPCResult::Type::STR_HEX, "hash_serialized_2", /* optional */ true, "The serialized hash (only present if 'hash_serialized_2' hash_type is chosen)"},
                        {RPCResult::Type::STR_HEX, "muhash", /* optional */ true, "The serialized hash (only present if 'muhash' hash_type is chosen)"},
                        {RPCResult::Type::NUM, "transactions", "The number of transactions with unspent outputs (not available when coinstatsindex is used)"},
                        {RPCResult::Type::NUM, "disk_size", "The estimated size of the chainstate on disk (not available when coinstatsindex is used)"},
                        {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of coins in the UTXO set"},
                        {RPCResult::Type::STR_AMOUNT, "total_unspendable_amount", "The total amount of coins permanently excluded from the UTXO set (only available if coinstatsindex is used)"},
                        {RPCResult::Type::OBJ, "block_info", "Info on amounts in the block at this block height (only available if coinstatsindex is used)",
                        {
                            {RPCResult::Type::STR_AMOUNT, "prevout_spent", ""},
                            {RPCResult::Type::STR_AMOUNT, "coinbase", ""},
                            {RPCResult::Type::STR_AMOUNT, "new_outputs_ex_coinbase", ""},
                            {RPCResult::Type::STR_AMOUNT, "unspendable", ""},
                            {RPCResult::Type::OBJ, "unspendables", "Detailed view of the unspendable categories",
                            {
                                {RPCResult::Type::STR_AMOUNT, "genesis_block", ""},
                                {RPCResult::Type::STR_AMOUNT, "bip30", "Transactions overridden by duplicates (no longer possible with BIP30)"},
                                {RPCResult::Type::STR_AMOUNT, "scripts", "Amounts sent to scripts that are unspendable (for example OP_RETURN outputs)"},
                                {RPCResult::Type::STR_AMOUNT, "unclaimed_rewards", "Fee rewards that miners did not claim in their coinbase transaction"},
                            }}
                        }},
                    }},
                RPCExamples{
                    HelpExampleCli("gettxoutsetinfo", "") +
                    HelpExampleCli("gettxoutsetinfo", R"("none")") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" 1000)") +
                    HelpExampleCli("gettxoutsetinfo", R"("none" '"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"')") +
                    HelpExampleRpc("gettxoutsetinfo", "") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none")") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", 1000)") +
                    HelpExampleRpc("gettxoutsetinfo", R"("none", "00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09")")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue ret(UniValue::VOBJ);

    CBlockIndex* pindex{nullptr};
    const CoinStatsHashType hash_type{request.params[0].isNull() ? CoinStatsHashType::HASH_SERIALIZED : ParseHashType(request.params[0].get_str())};
    CCoinsStats stats{hash_type};
    stats.index_requested = request.params[2].isNull() || request.params[2].get_bool();

    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    active_chainstate.ForceFlushStateToDisk();

    CCoinsView* coins_view;
    BlockManager* blockman;
    {
        LOCK(::cs_main);
        coins_view = &active_chainstate.CoinsDB();
        blockman = &active_chainstate.m_blockman;
        pindex = blockman->LookupBlockIndex(coins_view->GetBestBlock());
    }

    if (!request.params[1].isNull()) {
        if (!g_coin_stats_index) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Querying specific block heights requires coinstatsindex");
        }

        if (stats.m_hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "hash_serialized_2 hash type cannot be queried for a specific block");
        }

        pindex = ParseHashOrHeight(request.params[1], chainman);
    }

    if (GetUTXOStats(coins_view, *blockman, stats, node.rpc_interruption_point, pindex)) {
        ret.pushKV("height", (int64_t)stats.nHeight);
        ret.pushKV("bestblock", stats.hashBlock.GetHex());
        ret.pushKV("txouts", (int64_t)stats.nTransactionOutputs);
        ret.pushKV("bogosize", (int64_t)stats.nBogoSize);
        if (hash_type == CoinStatsHashType::HASH_SERIALIZED) {
            ret.pushKV("hash_serialized_2", stats.hashSerialized.GetHex());
        }
        if (hash_type == CoinStatsHashType::MUHASH) {
              ret.pushKV("muhash", stats.hashSerialized.GetHex());
        }
        ret.pushKV("total_amount", ValueFromAmount(stats.nTotalAmount));
        if (!stats.index_used) {
            ret.pushKV("transactions", static_cast<int64_t>(stats.nTransactions));
            ret.pushKV("disk_size", stats.nDiskSize);
        } else {
            ret.pushKV("total_unspendable_amount", ValueFromAmount(stats.block_unspendable_amount));

            CCoinsStats prev_stats{hash_type};

            if (pindex->nHeight > 0) {
                GetUTXOStats(coins_view, *blockman, prev_stats, node.rpc_interruption_point, pindex->pprev);
            }

            UniValue block_info(UniValue::VOBJ);
            block_info.pushKV("prevout_spent", ValueFromAmount(stats.block_prevout_spent_amount - prev_stats.block_prevout_spent_amount));
            block_info.pushKV("coinbase", ValueFromAmount(stats.block_coinbase_amount - prev_stats.block_coinbase_amount));
            block_info.pushKV("new_outputs_ex_coinbase", ValueFromAmount(stats.block_new_outputs_ex_coinbase_amount - prev_stats.block_new_outputs_ex_coinbase_amount));
            block_info.pushKV("unspendable", ValueFromAmount(stats.block_unspendable_amount - prev_stats.block_unspendable_amount));

            UniValue unspendables(UniValue::VOBJ);
            unspendables.pushKV("genesis_block", ValueFromAmount(stats.unspendables_genesis_block - prev_stats.unspendables_genesis_block));
            unspendables.pushKV("bip30", ValueFromAmount(stats.unspendables_bip30 - prev_stats.unspendables_bip30));
            unspendables.pushKV("scripts", ValueFromAmount(stats.unspendables_scripts - prev_stats.unspendables_scripts));
            unspendables.pushKV("unclaimed_rewards", ValueFromAmount(stats.unspendables_unclaimed_rewards - prev_stats.unspendables_unclaimed_rewards));
            block_info.pushKV("unspendables", unspendables);

            ret.pushKV("block_info", block_info);
        }
    } else {
        if (g_coin_stats_index) {
            const IndexSummary summary{g_coin_stats_index->GetSummary()};

            if (!summary.synced) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Unable to read UTXO set because coinstatsindex is still syncing. Current height: %d", summary.best_block_height));
            }
        }
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
    }
    return ret;
},
    };
}

static RPCHelpMan gettxout()
{
    return RPCHelpMan{"gettxout",
        "\nReturns details about an unspent transaction output.\n",
        {
            {"txid", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction id"},
            {"n", RPCArg::Type::NUM, RPCArg::Optional::NO, "vout number"},
            {"include_mempool", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to include the mempool. Note that an unspent output that is spent in the mempool won't appear."},
        },
        {
            RPCResult{"If the UTXO was not found", RPCResult::Type::NONE, "", ""},
            RPCResult{"Otherwise", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::NUM, "confirmations", "The number of confirmations"},
                {RPCResult::Type::STR_AMOUNT, "value", "The transaction value in " + CURRENCY_UNIT},
                {RPCResult::Type::OBJ, "scriptPubKey", "", {
                    {RPCResult::Type::STR, "asm", ""},
                    {RPCResult::Type::STR_HEX, "hex", ""},
                    {RPCResult::Type::NUM, "reqSigs", /* optional */ true, "(DEPRECATED, returned only if config option -deprecatedrpc=addresses is passed) Number of required signatures"},
                    {RPCResult::Type::STR, "type", "The type, eg pubkeyhash"},
                    {RPCResult::Type::STR, "address", /* optional */ true, "qtum address (only if a well-defined address exists)"},
                    {RPCResult::Type::ARR, "addresses", /* optional */ true, "(DEPRECATED, returned only if config option -deprecatedrpc=addresses is passed) Array of qtum addresses",
                        {{RPCResult::Type::STR, "address", "qtum address"}}},
                }},
                {RPCResult::Type::BOOL, "coinbase", "Coinbase or not"},
            }},
        },
        RPCExamples{
            "\nGet unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nView the details\n"
            + HelpExampleCli("gettxout", "\"txid\" 1") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("gettxout", "\"txid\", 1")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    LOCK(cs_main);

    UniValue ret(UniValue::VOBJ);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    int n = request.params[1].get_int();
    COutPoint out(hash, n);
    bool fMempool = true;
    if (!request.params[2].isNull())
        fMempool = request.params[2].get_bool();

    Coin coin;
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CCoinsViewCache* coins_view = &active_chainstate.CoinsTip();

    if (fMempool) {
        const CTxMemPool& mempool = EnsureMemPool(node);
        LOCK(mempool.cs);
        CCoinsViewMemPool view(coins_view, mempool);
        if (!view.GetCoin(out, coin) || mempool.isSpent(out)) {
            return NullUniValue;
        }
    } else {
        if (!coins_view->GetCoin(out, coin)) {
            return NullUniValue;
        }
    }

    const CBlockIndex* pindex = active_chainstate.m_blockman.LookupBlockIndex(coins_view->GetBestBlock());
    ret.pushKV("bestblock", pindex->GetBlockHash().GetHex());
    if (coin.nHeight == MEMPOOL_HEIGHT) {
        ret.pushKV("confirmations", 0);
    } else {
        ret.pushKV("confirmations", (int64_t)(pindex->nHeight - coin.nHeight + 1));
    }
    ret.pushKV("value", ValueFromAmount(coin.out.nValue));
    UniValue o(UniValue::VOBJ);
    ScriptPubKeyToUniv(coin.out.scriptPubKey, o, true);
    ret.pushKV("scriptPubKey", o);
    ret.pushKV("coinbase", (bool)coin.fCoinBase);
    ret.pushKV("coinstake", (bool)coin.fCoinStake);

    return ret;
},
    };
}

static RPCHelpMan verifychain()
{
    return RPCHelpMan{"verifychain",
                "\nVerifies blockchain database.\n",
                {
                    {"checklevel", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, range=0-4", DEFAULT_CHECKLEVEL)},
                        strprintf("How thorough the block verification is:\n - %s", Join(CHECKLEVEL_DOC, "\n- "))},
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{strprintf("%d, 0=all", DEFAULT_CHECKBLOCKS)}, "The number of blocks to check."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Verified or not"},
                RPCExamples{
                    HelpExampleCli("verifychain", "")
            + HelpExampleRpc("verifychain", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int check_level(request.params[0].isNull() ? DEFAULT_CHECKLEVEL : request.params[0].get_int());
    const int check_depth{request.params[1].isNull() ? DEFAULT_CHECKBLOCKS : request.params[1].get_int()};

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);

    CChainState& active_chainstate = chainman.ActiveChainstate();
    return CVerifyDB().VerifyDB(
        active_chainstate, Params(), active_chainstate.CoinsTip(), check_level, check_depth);
},
    };
}

static void SoftForkDescPushBack(const CBlockIndex* active_chain_tip, UniValue& softforks, const Consensus::Params& params, Consensus::BuriedDeployment dep)
{
    // For buried deployments.

    if (!DeploymentEnabled(params, dep)) return;

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "buried");
    // getblockchaininfo reports the softfork as active from when the chain height is
    // one below the activation height
    rv.pushKV("active", DeploymentActiveAfter(active_chain_tip, params, dep));
    rv.pushKV("height", params.DeploymentHeight(dep));
    softforks.pushKV(DeploymentName(dep), rv);
}

static void SoftForkDescPushBack(const CBlockIndex* active_chain_tip, UniValue& softforks, const Consensus::Params& consensusParams, Consensus::DeploymentPos id)
{
    // For BIP9 deployments.

    if (!DeploymentEnabled(consensusParams, id)) return;

    UniValue bip9(UniValue::VOBJ);
    const ThresholdState thresholdState = g_versionbitscache.State(active_chain_tip, consensusParams, id);
    switch (thresholdState) {
    case ThresholdState::DEFINED: bip9.pushKV("status", "defined"); break;
    case ThresholdState::STARTED: bip9.pushKV("status", "started"); break;
    case ThresholdState::LOCKED_IN: bip9.pushKV("status", "locked_in"); break;
    case ThresholdState::ACTIVE: bip9.pushKV("status", "active"); break;
    case ThresholdState::FAILED: bip9.pushKV("status", "failed"); break;
    }
    if (ThresholdState::STARTED == thresholdState)
    {
        bip9.pushKV("bit", consensusParams.vDeployments[id].bit);
    }
    bip9.pushKV("start_time", consensusParams.vDeployments[id].nStartTime);
    bip9.pushKV("timeout", consensusParams.vDeployments[id].nTimeout);
    int64_t since_height = g_versionbitscache.StateSinceHeight(active_chain_tip, consensusParams, id);
    bip9.pushKV("since", since_height);
    if (ThresholdState::STARTED == thresholdState)
    {
        UniValue statsUV(UniValue::VOBJ);
        BIP9Stats statsStruct = g_versionbitscache.Statistics(active_chain_tip, consensusParams, id);
        statsUV.pushKV("period", statsStruct.period);
        statsUV.pushKV("threshold", statsStruct.threshold);
        statsUV.pushKV("elapsed", statsStruct.elapsed);
        statsUV.pushKV("count", statsStruct.count);
        statsUV.pushKV("possible", statsStruct.possible);
        bip9.pushKV("statistics", statsUV);
    }
    bip9.pushKV("min_activation_height", consensusParams.vDeployments[id].min_activation_height);

    UniValue rv(UniValue::VOBJ);
    rv.pushKV("type", "bip9");
    rv.pushKV("bip9", bip9);
    if (ThresholdState::ACTIVE == thresholdState) {
        rv.pushKV("height", since_height);
    }
    rv.pushKV("active", ThresholdState::ACTIVE == thresholdState);

    softforks.pushKV(DeploymentName(id), rv);
}

RPCHelpMan getblockchaininfo()
{
    return RPCHelpMan{"getblockchaininfo",
                "Returns an object containing various state info regarding blockchain processing.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "chain", "current network name (main, test, signet, regtest)"},
                        {RPCResult::Type::NUM, "blocks", "the height of the most-work fully-validated chain. The genesis block has height 0"},
                        {RPCResult::Type::NUM, "headers", "the current number of headers we have validated"},
                        {RPCResult::Type::STR, "bestblockhash", "the hash of the currently best block"},
                        {RPCResult::Type::NUM, "difficulty", "the current difficulty"},
                        {RPCResult::Type::NUM, "mediantime", "median time for the current best block"},
                        {RPCResult::Type::NUM, "verificationprogress", "estimate of verification progress [0..1]"},
                        {RPCResult::Type::BOOL, "initialblockdownload", "(debug information) estimate of whether this node is in Initial Block Download mode"},
                        {RPCResult::Type::STR_HEX, "chainwork", "total amount of work in active chain, in hexadecimal"},
                        {RPCResult::Type::NUM, "size_on_disk", "the estimated size of the block and undo files on disk"},
                        {RPCResult::Type::BOOL, "pruned", "if the blocks are subject to pruning"},
                        {RPCResult::Type::NUM, "pruneheight", "lowest-height complete block stored (only present if pruning is enabled)"},
                        {RPCResult::Type::BOOL, "automatic_pruning", "whether automatic pruning is enabled (only present if pruning is enabled)"},
                        {RPCResult::Type::NUM, "prune_target_size", "the target size used by pruning (only present if automatic pruning is enabled)"},
                        {RPCResult::Type::OBJ_DYN, "softforks", "status of softforks",
                        {
                            {RPCResult::Type::OBJ, "xxxx", "name of the softfork",
                            {
                                {RPCResult::Type::STR, "type", "one of \"buried\", \"bip9\""},
                                {RPCResult::Type::OBJ, "bip9", "status of bip9 softforks (only for \"bip9\" type)",
                                {
                                    {RPCResult::Type::STR, "status", "one of \"defined\", \"started\", \"locked_in\", \"active\", \"failed\""},
                                    {RPCResult::Type::NUM, "bit", "the bit (0-28) in the block version field used to signal this softfork (only for \"started\" status)"},
                                    {RPCResult::Type::NUM_TIME, "start_time", "the minimum median time past of a block at which the bit gains its meaning"},
                                    {RPCResult::Type::NUM_TIME, "timeout", "the median time past of a block at which the deployment is considered failed if not yet locked in"},
                                    {RPCResult::Type::NUM, "since", "height of the first block to which the status applies"},
                                    {RPCResult::Type::NUM, "min_activation_height", "minimum height of blocks for which the rules may be enforced"},
                                    {RPCResult::Type::OBJ, "statistics", "numeric statistics about BIP9 signalling for a softfork (only for \"started\" status)",
                                    {
                                        {RPCResult::Type::NUM, "period", "the length in blocks of the BIP9 signalling period"},
                                        {RPCResult::Type::NUM, "threshold", "the number of blocks with the version bit set required to activate the feature"},
                                        {RPCResult::Type::NUM, "elapsed", "the number of blocks elapsed since the beginning of the current period"},
                                        {RPCResult::Type::NUM, "count", "the number of blocks with the version bit set in the current period"},
                                        {RPCResult::Type::BOOL, "possible", "returns false if there are not enough blocks left in this period to pass activation threshold"},
                                    }},
                                }},
                                {RPCResult::Type::NUM, "height", "height of the first block which the rules are or will be enforced (only for \"buried\" type, or \"bip9\" type with \"active\" status)"},
                                {RPCResult::Type::BOOL, "active", "true if the rules are enforced for the mempool and the next block"},
                            }},
                        }},
                        {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockchaininfo", "")
            + HelpExampleRpc("getblockchaininfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();

    const CBlockIndex* tip = active_chainstate.m_chain.Tip();
    CHECK_NONFATAL(tip);
    const int height = tip->nHeight;
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("chain",                 Params().NetworkIDString());
    obj.pushKV("blocks",                height);
    obj.pushKV("headers",               pindexBestHeader ? pindexBestHeader->nHeight : -1);
    obj.pushKV("bestblockhash",         tip->GetBlockHash().GetHex());
    obj.pushKV("difficulty",            (double)GetDifficulty(tip));
    obj.pushKV("moneysupply",           pindexBestHeader->nMoneySupply / COIN);
    obj.pushKV("mediantime",            (int64_t)tip->GetMedianTimePast());
    obj.pushKV("verificationprogress",  GuessVerificationProgress(Params().TxData(), tip));
    obj.pushKV("initialblockdownload",  active_chainstate.IsInitialBlockDownload());
    obj.pushKV("chainwork",             tip->nChainWork.GetHex());
    obj.pushKV("size_on_disk",          CalculateCurrentUsage());
    obj.pushKV("pruned",                fPruneMode);
    if (fPruneMode) {
        const CBlockIndex* block = tip;
        CHECK_NONFATAL(block);
        while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
            block = block->pprev;
        }

        obj.pushKV("pruneheight",        block->nHeight);

        // if 0, execution bypasses the whole if block.
        bool automatic_pruning = (gArgs.GetArg("-prune", 0) != 1);
        obj.pushKV("automatic_pruning",  automatic_pruning);
        if (automatic_pruning) {
            obj.pushKV("prune_target_size",  nPruneTarget);
        }
    }

    const Consensus::Params& consensusParams = Params().GetConsensus();
    UniValue softforks(UniValue::VOBJ);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_HEIGHTINCB);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_DERSIG);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_CLTV);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_TESTDUMMY);
    SoftForkDescPushBack(tip, softforks, consensusParams, Consensus::DEPLOYMENT_TAPROOT);
    obj.pushKV("softforks", softforks);

    obj.pushKV("warnings", GetWarnings(false).original);
    return obj;
},
    };
}

/** Comparison function for sorting the getchaintips heads.  */

static RPCHelpMan getchaintips()
{
    return RPCHelpMan{"getchaintips",
                "Return information about all known tips in the block tree,"
                " including the main chain as well as orphaned branches.\n",
                {},
                RPCResult{
                    RPCResult::Type::ARR, "", "",
                    {{RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::NUM, "height", "height of the chain tip"},
                            {RPCResult::Type::STR_HEX, "hash", "block hash of the tip"},
                            {RPCResult::Type::NUM, "branchlen", "zero for main chain, otherwise length of branch connecting the tip to the main chain"},
                            {RPCResult::Type::STR, "status", "status of the chain, \"active\" for the main chain\n"
            "Possible values for status:\n"
            "1.  \"invalid\"               This branch contains at least one invalid block\n"
            "2.  \"headers-only\"          Not all blocks for this branch are available, but the headers are valid\n"
            "3.  \"valid-headers\"         All blocks are available for this branch, but they were never fully validated\n"
            "4.  \"valid-fork\"            This branch is not part of the active chain, but is fully validated\n"
            "5.  \"active\"                This is the tip of the active main chain, which is certainly valid"},
                        }}}},
                RPCExamples{
                    HelpExampleCli("getchaintips", "")
            + HelpExampleRpc("getchaintips", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CChain& active_chain = chainman.ActiveChain();

    /*
     * Idea: The set of chain tips is the active chain tip, plus orphan blocks which do not have another orphan building off of them.
     * Algorithm:
     *  - Make one pass through BlockIndex(), picking out the orphan blocks, and also storing a set of the orphan block's pprev pointers.
     *  - Iterate through the orphan blocks. If the block isn't pointed to by another orphan, it is a chain tip.
     *  - Add the active chain tip
     */
    std::set<const CBlockIndex*, CompareBlocksByHeight> setTips;
    std::set<const CBlockIndex*> setOrphans;
    std::set<const CBlockIndex*> setPrevs;

    for (const std::pair<const uint256, CBlockIndex*>& item : chainman.BlockIndex()) {
        if (!active_chain.Contains(item.second)) {
            setOrphans.insert(item.second);
            setPrevs.insert(item.second->pprev);
        }
    }

    for (std::set<const CBlockIndex*>::iterator it = setOrphans.begin(); it != setOrphans.end(); ++it) {
        if (setPrevs.erase(*it) == 0) {
            setTips.insert(*it);
        }
    }

    // Always report the currently active tip.
    setTips.insert(active_chain.Tip());

    /* Construct the output array.  */
    UniValue res(UniValue::VARR);
    for (const CBlockIndex* block : setTips) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("height", block->nHeight);
        obj.pushKV("hash", block->phashBlock->GetHex());

        const int branchLen = block->nHeight - active_chain.FindFork(block)->nHeight;
        obj.pushKV("branchlen", branchLen);

        std::string status;
        if (active_chain.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->nStatus & BLOCK_FAILED_MASK) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (!block->HaveTxsDownloaded()) {
            // This block cannot be connected because full block data for it or one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This block is fully validated, but no longer part of the active chain. It was probably the active block once, but was reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BLOCK_VALID_TREE)) {
            // The headers for this block are valid, but it has not been validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        obj.pushKV("status", status);

        res.push_back(obj);
    }

    return res;
},
    };
}

UniValue MempoolInfoToJSON(const CTxMemPool& pool)
{
    // Make sure this call is atomic in the pool.
    LOCK(pool.cs);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("loaded", pool.IsLoaded());
    ret.pushKV("size", (int64_t)pool.size());
    ret.pushKV("bytes", (int64_t)pool.GetTotalTxSize());
    ret.pushKV("usage", (int64_t)pool.DynamicMemoryUsage());
    ret.pushKV("total_fee", ValueFromAmount(pool.GetTotalFee()));
    size_t maxmempool = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    ret.pushKV("maxmempool", (int64_t) maxmempool);
    ret.pushKV("mempoolminfee", ValueFromAmount(std::max(pool.GetMinFee(maxmempool), ::minRelayTxFee).GetFeePerK()));
    ret.pushKV("minrelaytxfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));
    ret.pushKV("unbroadcastcount", uint64_t{pool.GetUnbroadcastTxs().size()});
    return ret;
}

static RPCHelpMan getmempoolinfo()
{
    return RPCHelpMan{"getmempoolinfo",
                "\nReturns details on the active state of the TX memory pool.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "loaded", "True if the mempool is fully loaded"},
                        {RPCResult::Type::NUM, "size", "Current tx count"},
                        {RPCResult::Type::NUM, "bytes", "Sum of all virtual transaction sizes as defined in BIP 141. Differs from actual serialized size because witness data is discounted"},
                        {RPCResult::Type::NUM, "usage", "Total memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "total_fee", "Total fees for the mempool in " + CURRENCY_UNIT + ", ignoring modified fees through prioritizetransaction"},
                        {RPCResult::Type::NUM, "maxmempool", "Maximum memory usage for the mempool"},
                        {RPCResult::Type::STR_AMOUNT, "mempoolminfee", "Minimum fee rate in " + CURRENCY_UNIT + "/kvB for tx to be accepted. Is the maximum of minrelaytxfee and minimum mempool fee"},
                        {RPCResult::Type::STR_AMOUNT, "minrelaytxfee", "Current minimum relay fee for transactions"},
                        {RPCResult::Type::NUM, "unbroadcastcount", "Current number of transactions that haven't passed initial broadcast yet"}
                    }},
                RPCExamples{
                    HelpExampleCli("getmempoolinfo", "")
            + HelpExampleRpc("getmempoolinfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return MempoolInfoToJSON(EnsureAnyMemPool(request.context));
},
    };
}

static RPCHelpMan preciousblock()
{
    return RPCHelpMan{"preciousblock",
                "\nTreats a block as if it were received before others with the same work.\n"
                "\nA later preciousblock call can override the effect of an earlier one.\n"
                "\nThe effects of preciousblock are not retained across restarts.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as precious"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("preciousblock", "\"blockhash\"")
            + HelpExampleRpc("preciousblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    CBlockIndex* pblockindex;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    BlockValidationState state;
    chainman.ActiveChainstate().PreciousBlock(state, pblockindex);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan invalidateblock()
{
    return RPCHelpMan{"invalidateblock",
                "\nPermanently marks a block as invalid, as if it violated a consensus rule.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to mark as invalid"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("invalidateblock", "\"blockhash\"")
            + HelpExampleRpc("invalidateblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 hash(ParseHashV(request.params[0], "blockhash"));
    BlockValidationState state;

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CBlockIndex* pblockindex;
    {
        LOCK(cs_main);
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }
    chainman.ActiveChainstate().InvalidateBlock(state, pblockindex);

    if (state.IsValid()) {
        chainman.ActiveChainstate().ActivateBestChain(state);
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan reconsiderblock()
{
    return RPCHelpMan{"reconsiderblock",
                "\nRemoves invalidity status of a block, its ancestors and its descendants, reconsider them for activation.\n"
                "This can be used to undo the effects of invalidateblock.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hash of the block to reconsider"},
                },
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("reconsiderblock", "\"blockhash\"")
            + HelpExampleRpc("reconsiderblock", "\"blockhash\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    uint256 hash(ParseHashV(request.params[0], "blockhash"));

    {
        LOCK(cs_main);
        CBlockIndex* pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }

        chainman.ActiveChainstate().ResetBlockFailureFlags(pblockindex);
    }

    BlockValidationState state;
    chainman.ActiveChainstate().ActivateBestChain(state);

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.ToString());
    }

    return NullUniValue;
},
    };
}

static RPCHelpMan getchaintxstats()
{
    return RPCHelpMan{"getchaintxstats",
                "\nCompute statistics about the total number and rate of transactions in the chain.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::DefaultHint{"one month"}, "Size of the window in number of blocks"},
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"chain tip"}, "The hash of the block that ends the window."},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM_TIME, "time", "The timestamp for the final block in the window, expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::NUM, "txcount", "The total number of transactions in the chain up to that point"},
                        {RPCResult::Type::STR_HEX, "window_final_block_hash", "The hash of the final block in the window"},
                        {RPCResult::Type::NUM, "window_final_block_height", "The height of the final block in the window."},
                        {RPCResult::Type::NUM, "window_block_count", "Size of the window in number of blocks"},
                        {RPCResult::Type::NUM, "window_tx_count", /* optional */ true, "The number of transactions in the window. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "window_interval", /* optional */ true, "The elapsed time in the window in seconds. Only returned if \"window_block_count\" is > 0"},
                        {RPCResult::Type::NUM, "txrate", /* optional */ true, "The average rate of transactions per second in the window. Only returned if \"window_interval\" is > 0"},
                    }},
                RPCExamples{
                    HelpExampleCli("getchaintxstats", "")
            + HelpExampleRpc("getchaintxstats", "2016")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* pindex;

    if (request.params[1].isNull()) {
        LOCK(cs_main);
        pindex = chainman.ActiveChain().Tip();
    } else {
        uint256 hash(ParseHashV(request.params[1], "blockhash"));
        LOCK(cs_main);
        pindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pindex) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        if (!chainman.ActiveChain().Contains(pindex)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block is not in main chain");
        }
    }
    int blockcount = 30 * 24 * 60 * 60 / Params().GetConsensus().TargetSpacing(pindex->nHeight); // By default: 1 month

    CHECK_NONFATAL(pindex != nullptr);

    if (request.params[0].isNull()) {
        blockcount = std::max(0, std::min(blockcount, pindex->nHeight - 1));
    } else {
        blockcount = request.params[0].get_int();

        if (blockcount < 0 || (blockcount > 0 && blockcount >= pindex->nHeight)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid block count: should be between 0 and the block's height - 1");
        }
    }

    const CBlockIndex* pindexPast = pindex->GetAncestor(pindex->nHeight - blockcount);
    int nTimeDiff = pindex->GetMedianTimePast() - pindexPast->GetMedianTimePast();
    int nTxDiff = pindex->nChainTx - pindexPast->nChainTx;

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("time", (int64_t)pindex->nTime);
    ret.pushKV("txcount", (int64_t)pindex->nChainTx);
    ret.pushKV("window_final_block_hash", pindex->GetBlockHash().GetHex());
    ret.pushKV("window_final_block_height", pindex->nHeight);
    ret.pushKV("window_block_count", blockcount);
    if (blockcount > 0) {
        ret.pushKV("window_tx_count", nTxDiff);
        ret.pushKV("window_interval", nTimeDiff);
        if (nTimeDiff > 0) {
            ret.pushKV("txrate", ((double)nTxDiff) / nTimeDiff);
        }
    }

    return ret;
},
    };
}

template<typename T>
static T CalculateTruncatedMedian(std::vector<T>& scores)
{
    size_t size = scores.size();
    if (size == 0) {
        return 0;
    }

    std::sort(scores.begin(), scores.end());
    if (size % 2 == 0) {
        return (scores[size / 2 - 1] + scores[size / 2]) / 2;
    } else {
        return scores[size / 2];
    }
}

void CalculatePercentilesByWeight(CAmount result[NUM_GETBLOCKSTATS_PERCENTILES], std::vector<std::pair<CAmount, int64_t>>& scores, int64_t total_weight)
{
    if (scores.empty()) {
        return;
    }

    std::sort(scores.begin(), scores.end());

    // 10th, 25th, 50th, 75th, and 90th percentile weight units.
    const double weights[NUM_GETBLOCKSTATS_PERCENTILES] = {
        total_weight / 10.0, total_weight / 4.0, total_weight / 2.0, (total_weight * 3.0) / 4.0, (total_weight * 9.0) / 10.0
    };

    int64_t next_percentile_index = 0;
    int64_t cumulative_weight = 0;
    for (const auto& element : scores) {
        cumulative_weight += element.second;
        while (next_percentile_index < NUM_GETBLOCKSTATS_PERCENTILES && cumulative_weight >= weights[next_percentile_index]) {
            result[next_percentile_index] = element.first;
            ++next_percentile_index;
        }
    }

    // Fill any remaining percentiles with the last value.
    for (int64_t i = next_percentile_index; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        result[i] = scores.back().first;
    }
}

void ScriptPubKeyToUniv(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex)
{
    ScriptPubKeyToUniv(scriptPubKey, out, fIncludeHex, IsDeprecatedRPCEnabled("addresses"));
}

void TxToUniv(const CTransaction& tx, const uint256& hashBlock, UniValue& entry, bool include_hex, int serialize_flags, const CTxUndo* txundo)
{
    TxToUniv(tx, hashBlock, IsDeprecatedRPCEnabled("addresses"), entry, include_hex, serialize_flags, txundo);
}

template<typename T>
static inline bool SetHasKeys(const std::set<T>& set) {return false;}
template<typename T, typename Tk, typename... Args>
static inline bool SetHasKeys(const std::set<T>& set, const Tk& key, const Args&... args)
{
    return (set.count(key) != 0) || SetHasKeys(set, args...);
}

// outpoint (needed for the utxo index) + nHeight + fCoinBase
static constexpr size_t PER_UTXO_OVERHEAD = sizeof(COutPoint) + sizeof(uint32_t) + sizeof(bool);

static RPCHelpMan getblockstats()
{
    return RPCHelpMan{"getblockstats",
                "\nCompute per block statistics for a given window. All amounts are in satoshis.\n"
                "It won't work for some heights with pruning.\n",
                {
                    {"hash_or_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "The block hash or height of the target block", "", {"", "string or numeric"}},
                    {"stats", RPCArg::Type::ARR, RPCArg::DefaultHint{"all values"}, "Values to plot (see result below)",
                        {
                            {"height", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                            {"time", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Selected statistic"},
                        },
                        "stats"},
                },
                RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "avgfee", "Average fee in the block"},
                {RPCResult::Type::NUM, "avgfeerate", "Average feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "avgtxsize", "Average transaction size"},
                {RPCResult::Type::STR_HEX, "blockhash", "The block hash (to check for potential reorgs)"},
                {RPCResult::Type::ARR_FIXED, "feerate_percentiles", "Feerates at the 10th, 25th, 50th, 75th, and 90th percentile weight unit (in satoshis per virtual byte)",
                {
                    {RPCResult::Type::NUM, "10th_percentile_feerate", "The 10th percentile feerate"},
                    {RPCResult::Type::NUM, "25th_percentile_feerate", "The 25th percentile feerate"},
                    {RPCResult::Type::NUM, "50th_percentile_feerate", "The 50th percentile feerate"},
                    {RPCResult::Type::NUM, "75th_percentile_feerate", "The 75th percentile feerate"},
                    {RPCResult::Type::NUM, "90th_percentile_feerate", "The 90th percentile feerate"},
                }},
                {RPCResult::Type::NUM, "height", "The height of the block"},
                {RPCResult::Type::NUM, "ins", "The number of inputs (excluding coinbase)"},
                {RPCResult::Type::NUM, "maxfee", "Maximum fee in the block"},
                {RPCResult::Type::NUM, "maxfeerate", "Maximum feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "maxtxsize", "Maximum transaction size"},
                {RPCResult::Type::NUM, "medianfee", "Truncated median fee in the block"},
                {RPCResult::Type::NUM, "mediantime", "The block median time past"},
                {RPCResult::Type::NUM, "mediantxsize", "Truncated median transaction size"},
                {RPCResult::Type::NUM, "minfee", "Minimum fee in the block"},
                {RPCResult::Type::NUM, "minfeerate", "Minimum feerate (in satoshis per virtual byte)"},
                {RPCResult::Type::NUM, "mintxsize", "Minimum transaction size"},
                {RPCResult::Type::NUM, "outs", "The number of outputs"},
                {RPCResult::Type::NUM, "subsidy", "The block subsidy"},
                {RPCResult::Type::NUM, "swtotal_size", "Total size of all segwit transactions"},
                {RPCResult::Type::NUM, "swtotal_weight", "Total weight of all segwit transactions"},
                {RPCResult::Type::NUM, "swtxs", "The number of segwit transactions"},
                {RPCResult::Type::NUM, "time", "The block time"},
                {RPCResult::Type::NUM, "total_out", "Total amount in all outputs (excluding coinbase and thus reward [ie subsidy + totalfee])"},
                {RPCResult::Type::NUM, "total_size", "Total size of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "total_weight", "Total weight of all non-coinbase transactions"},
                {RPCResult::Type::NUM, "totalfee", "The fee total"},
                {RPCResult::Type::NUM, "txs", "The number of transactions (including coinbase)"},
                {RPCResult::Type::NUM, "utxo_increase", "The increase/decrease in the number of unspent outputs"},
                {RPCResult::Type::NUM, "utxo_size_inc", "The increase/decrease in size for the utxo index (not discounting op_return and similar)"},
            }},
                RPCExamples{
                    HelpExampleCli("getblockstats", R"('"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09"' '["minfeerate","avgfeerate"]')") +
                    HelpExampleCli("getblockstats", R"(1000 '["minfeerate","avgfeerate"]')") +
                    HelpExampleRpc("getblockstats", R"("00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09", ["minfeerate","avgfeerate"])") +
                    HelpExampleRpc("getblockstats", R"(1000, ["minfeerate","avgfeerate"])")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    CBlockIndex* pindex{ParseHashOrHeight(request.params[0], chainman)};
    CHECK_NONFATAL(pindex != nullptr);

    std::set<std::string> stats;
    if (!request.params[1].isNull()) {
        const UniValue stats_univalue = request.params[1].get_array();
        for (unsigned int i = 0; i < stats_univalue.size(); i++) {
            const std::string stat = stats_univalue[i].get_str();
            stats.insert(stat);
        }
    }

    const CBlock block = GetBlockChecked(pindex);
    const CBlockUndo blockUndo = GetUndoChecked(pindex);

    const bool do_all = stats.size() == 0; // Calculate everything if nothing selected (default)
    const bool do_mediantxsize = do_all || stats.count("mediantxsize") != 0;
    const bool do_medianfee = do_all || stats.count("medianfee") != 0;
    const bool do_feerate_percentiles = do_all || stats.count("feerate_percentiles") != 0;
    const bool loop_inputs = do_all || do_medianfee || do_feerate_percentiles ||
        SetHasKeys(stats, "utxo_size_inc", "totalfee", "avgfee", "avgfeerate", "minfee", "maxfee", "minfeerate", "maxfeerate");
    const bool loop_outputs = do_all || loop_inputs || stats.count("total_out");
    const bool do_calculate_size = do_mediantxsize ||
        SetHasKeys(stats, "total_size", "avgtxsize", "mintxsize", "maxtxsize", "swtotal_size");
    const bool do_calculate_weight = do_all || SetHasKeys(stats, "total_weight", "avgfeerate", "swtotal_weight", "avgfeerate", "feerate_percentiles", "minfeerate", "maxfeerate");
    const bool do_calculate_sw = do_all || SetHasKeys(stats, "swtxs", "swtotal_size", "swtotal_weight");

    CAmount maxfee = 0;
    CAmount maxfeerate = 0;
    CAmount minfee = MAX_MONEY;
    CAmount minfeerate = MAX_MONEY;
    CAmount total_out = 0;
    CAmount totalfee = 0;
    int64_t inputs = 0;
    int64_t maxtxsize = 0;
    int64_t mintxsize = dgpMaxBlockSerSize;
    int64_t outputs = 0;
    int64_t swtotal_size = 0;
    int64_t swtotal_weight = 0;
    int64_t swtxs = 0;
    int64_t total_size = 0;
    int64_t total_weight = 0;
    int64_t utxo_size_inc = 0;
    std::vector<CAmount> fee_array;
    std::vector<std::pair<CAmount, int64_t>> feerate_array;
    std::vector<int64_t> txsize_array;

    for (size_t i = 0; i < block.vtx.size(); ++i) {
        const auto& tx = block.vtx.at(i);
        outputs += tx->vout.size();

        CAmount tx_total_out = 0;
        if (loop_outputs) {
            for (const CTxOut& out : tx->vout) {
                tx_total_out += out.nValue;
                utxo_size_inc += GetSerializeSize(out, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
            }
        }

        if (tx->IsCoinBase() || tx->IsCoinStake()) {
            continue;
        }

        inputs += tx->vin.size(); // Don't count coinbase's fake input
        total_out += tx_total_out; // Don't count coinbase reward

        int64_t tx_size = 0;
        if (do_calculate_size) {

            tx_size = tx->GetTotalSize();
            if (do_mediantxsize) {
                txsize_array.push_back(tx_size);
            }
            maxtxsize = std::max(maxtxsize, tx_size);
            mintxsize = std::min(mintxsize, tx_size);
            total_size += tx_size;
        }

        int64_t weight = 0;
        if (do_calculate_weight) {
            weight = GetTransactionWeight(*tx);
            total_weight += weight;
        }

        if (do_calculate_sw && tx->HasWitness()) {
            ++swtxs;
            swtotal_size += tx_size;
            swtotal_weight += weight;
        }

        if (loop_inputs) {
            CAmount tx_total_in = 0;
            const auto& txundo = blockUndo.vtxundo.at(i - 1);
            for (const Coin& coin: txundo.vprevout) {
                const CTxOut& prevoutput = coin.out;

                tx_total_in += prevoutput.nValue;
                utxo_size_inc -= GetSerializeSize(prevoutput, PROTOCOL_VERSION) + PER_UTXO_OVERHEAD;
            }

            CAmount txfee = tx_total_in - tx_total_out;
            CHECK_NONFATAL(MoneyRange(txfee));
            if (do_medianfee) {
                fee_array.push_back(txfee);
            }
            maxfee = std::max(maxfee, txfee);
            minfee = std::min(minfee, txfee);
            totalfee += txfee;

            // New feerate uses satoshis per virtual byte instead of per serialized byte
            CAmount feerate = weight ? (txfee * WITNESS_SCALE_FACTOR) / weight : 0;
            if (do_feerate_percentiles) {
                feerate_array.emplace_back(std::make_pair(feerate, weight));
            }
            maxfeerate = std::max(maxfeerate, feerate);
            minfeerate = std::min(minfeerate, feerate);
        }
    }

    CAmount feerate_percentiles[NUM_GETBLOCKSTATS_PERCENTILES] = { 0 };
    CalculatePercentilesByWeight(feerate_percentiles, feerate_array, total_weight);

    UniValue feerates_res(UniValue::VARR);
    for (int64_t i = 0; i < NUM_GETBLOCKSTATS_PERCENTILES; i++) {
        feerates_res.push_back(feerate_percentiles[i]);
    }

    UniValue ret_all(UniValue::VOBJ);
    ret_all.pushKV("avgfee", (block.vtx.size() > 1) ? totalfee / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("avgfeerate", total_weight ? (totalfee * WITNESS_SCALE_FACTOR) / total_weight : 0); // Unit: sat/vbyte
    ret_all.pushKV("avgtxsize", (block.vtx.size() > 1) ? total_size / (block.vtx.size() - 1) : 0);
    ret_all.pushKV("blockhash", pindex->GetBlockHash().GetHex());
    ret_all.pushKV("feerate_percentiles", feerates_res);
    ret_all.pushKV("height", (int64_t)pindex->nHeight);
    ret_all.pushKV("ins", inputs);
    ret_all.pushKV("maxfee", maxfee);
    ret_all.pushKV("maxfeerate", maxfeerate);
    ret_all.pushKV("maxtxsize", maxtxsize);
    ret_all.pushKV("medianfee", CalculateTruncatedMedian(fee_array));
    ret_all.pushKV("mediantime", pindex->GetMedianTimePast());
    ret_all.pushKV("mediantxsize", CalculateTruncatedMedian(txsize_array));
    ret_all.pushKV("minfee", (minfee == MAX_MONEY) ? 0 : minfee);
    ret_all.pushKV("minfeerate", (minfeerate == MAX_MONEY) ? 0 : minfeerate);
    ret_all.pushKV("mintxsize", mintxsize == dgpMaxBlockSerSize ? 0 : mintxsize);
    ret_all.pushKV("outs", outputs);
    ret_all.pushKV("subsidy", GetBlockSubsidy(pindex->nHeight, Params().GetConsensus()));
    ret_all.pushKV("swtotal_size", swtotal_size);
    ret_all.pushKV("swtotal_weight", swtotal_weight);
    ret_all.pushKV("swtxs", swtxs);
    ret_all.pushKV("time", pindex->GetBlockTime());
    ret_all.pushKV("total_out", total_out);
    ret_all.pushKV("total_size", total_size);
    ret_all.pushKV("total_weight", total_weight);
    ret_all.pushKV("totalfee", totalfee);
    ret_all.pushKV("txs", (int64_t)block.vtx.size());
    ret_all.pushKV("utxo_increase", outputs - inputs);
    ret_all.pushKV("utxo_size_inc", utxo_size_inc);

    if (do_all) {
        return ret_all;
    }

    UniValue ret(UniValue::VOBJ);
    for (const std::string& stat : stats) {
        const UniValue& value = ret_all[stat];
        if (value.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid selected statistic %s", stat));
        }
        ret.pushKV(stat, value);
    }
    return ret;
},
    };
}

static RPCHelpMan savemempool()
{
    return RPCHelpMan{"savemempool",
                "\nDumps the mempool to disk. It will fail until the previous dump is fully loaded.\n",
                {},
                RPCResult{RPCResult::Type::NONE, "", ""},
                RPCExamples{
                    HelpExampleCli("savemempool", "")
            + HelpExampleRpc("savemempool", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const CTxMemPool& mempool = EnsureAnyMemPool(request.context);

    if (!mempool.IsLoaded()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The mempool was not loaded yet");
    }

    if (!DumpMempool(mempool)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Unable to dump mempool to disk");
    }

    return NullUniValue;
},
    };
}

namespace {
//! Search for a given set of pubkey scripts
bool FindScriptPubKey(std::atomic<int>& scan_progress, const std::atomic<bool>& should_abort, int64_t& count, CCoinsViewCursor* cursor, const std::set<CScript>& needles, std::map<COutPoint, Coin>& out_results, std::function<void()>& interruption_point)
{
    scan_progress = 0;
    count = 0;
    while (cursor->Valid()) {
        COutPoint key;
        Coin coin;
        if (!cursor->GetKey(key) || !cursor->GetValue(coin)) return false;
        if (++count % 8192 == 0) {
            interruption_point();
            if (should_abort) {
                // allow to abort the scan via the abort reference
                return false;
            }
        }
        if (count % 256 == 0) {
            // update progress reference every 256 item
            uint32_t high = 0x100 * *key.hash.begin() + *(key.hash.begin() + 1);
            scan_progress = (int)(high * 100.0 / 65536.0 + 0.5);
        }
        if (needles.count(coin.out.scriptPubKey)) {
            out_results.emplace(key, coin);
        }
        cursor->Next();
    }
    scan_progress = 100;
    return true;
}
} // namespace

/** RAII object to prevent concurrency issue when scanning the txout set */
static std::atomic<int> g_scan_progress;
static std::atomic<bool> g_scan_in_progress;
static std::atomic<bool> g_should_abort_scan;
class CoinsViewScanReserver
{
private:
    bool m_could_reserve;
public:
    explicit CoinsViewScanReserver() : m_could_reserve(false) {}

    bool reserve() {
        CHECK_NONFATAL(!m_could_reserve);
        if (g_scan_in_progress.exchange(true)) {
            return false;
        }
        CHECK_NONFATAL(g_scan_progress == 0);
        m_could_reserve = true;
        return true;
    }

    ~CoinsViewScanReserver() {
        if (m_could_reserve) {
            g_scan_in_progress = false;
            g_scan_progress = 0;
        }
    }
};

static RPCHelpMan scantxoutset()
{
    return RPCHelpMan{"scantxoutset",
        "\nScans the unspent transaction output set for entries that match certain output descriptors.\n"
        "Examples of output descriptors are:\n"
        "    addr(<address>)                      Outputs whose scriptPubKey corresponds to the specified address (does not include P2PK)\n"
        "    raw(<hex script>)                    Outputs whose scriptPubKey equals the specified hex scripts\n"
        "    combo(<pubkey>)                      P2PK, P2PKH, P2WPKH, and P2SH-P2WPKH outputs for the given pubkey\n"
        "    pkh(<pubkey>)                        P2PKH outputs for the given pubkey\n"
        "    sh(multi(<n>,<pubkey>,<pubkey>,...)) P2SH-multisig outputs for the given threshold and pubkeys\n"
        "\nIn the above, <pubkey> either refers to a fixed public key in hexadecimal notation, or to an xpub/xprv optionally followed by one\n"
        "or more path elements separated by \"/\", and optionally ending in \"/*\" (unhardened), or \"/*'\" or \"/*h\" (hardened) to specify all\n"
        "unhardened or hardened child keys.\n"
        "In the latter case, a range needs to be specified by below if different from 1000.\n"
        "For more information on output descriptors, see the documentation in the doc/descriptors.md file.\n",
        {
            {"action", RPCArg::Type::STR, RPCArg::Optional::NO, "The action to execute\n"
                "\"start\" for starting a scan\n"
                "\"abort\" for aborting the current scan (returns true when abort was successful)\n"
                "\"status\" for progress report (in %) of the current scan"},
            {"scanobjects", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Array of scan objects. Required for \"start\" action\n"
                "Every scan object is either a string descriptor or an object:",
            {
                {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "An output descriptor"},
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "An object with output descriptor and metadata",
                {
                    {"desc", RPCArg::Type::STR, RPCArg::Optional::NO, "An output descriptor"},
                    {"range", RPCArg::Type::RANGE, RPCArg::Default{1000}, "The range of HD chain indexes to explore (either end or [begin,end])"},
                }},
            },
                        "[scanobjects,...]"},
        },
        {
            RPCResult{"When action=='abort'", RPCResult::Type::BOOL, "", ""},
            RPCResult{"When action=='status' and no scan is in progress", RPCResult::Type::NONE, "", ""},
            RPCResult{"When action=='status' and scan is in progress", RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "progress", "The scan progress"},
            }},
            RPCResult{"When action=='start'", RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::BOOL, "success", "Whether the scan was completed"},
                {RPCResult::Type::NUM, "txouts", "The number of unspent transaction outputs scanned"},
                {RPCResult::Type::NUM, "height", "The current block height (index)"},
                {RPCResult::Type::STR_HEX, "bestblock", "The hash of the block at the tip of the chain"},
                {RPCResult::Type::ARR, "unspents", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                        {RPCResult::Type::NUM, "vout", "The vout value"},
                        {RPCResult::Type::STR_HEX, "scriptPubKey", "The script key"},
                        {RPCResult::Type::STR, "desc", "A specialized descriptor for the matched scriptPubKey"},
                        {RPCResult::Type::STR_AMOUNT, "amount", "The total amount in " + CURRENCY_UNIT + " of the unspent output"},
                        {RPCResult::Type::NUM, "height", "Height of the unspent transaction output"},
                    }},
                }},
                {RPCResult::Type::STR_AMOUNT, "total_amount", "The total amount of all found unspent outputs in " + CURRENCY_UNIT},
            }},
        },
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VARR});

    UniValue result(UniValue::VOBJ);
    if (request.params[0].get_str() == "status") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // no scan in progress
            return NullUniValue;
        }
        result.pushKV("progress", g_scan_progress);
        return result;
    } else if (request.params[0].get_str() == "abort") {
        CoinsViewScanReserver reserver;
        if (reserver.reserve()) {
            // reserve was possible which means no scan was running
            return false;
        }
        // set the abort flag
        g_should_abort_scan = true;
        return true;
    } else if (request.params[0].get_str() == "start") {
        CoinsViewScanReserver reserver;
        if (!reserver.reserve()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Scan already in progress, use action \"abort\" or \"status\"");
        }

        if (request.params.size() < 2) {
            throw JSONRPCError(RPC_MISC_ERROR, "scanobjects argument is required for the start action");
        }

        std::set<CScript> needles;
        std::map<CScript, std::string> descriptors;
        CAmount total_in = 0;

        // loop through the scan objects
        for (const UniValue& scanobject : request.params[1].get_array().getValues()) {
            FlatSigningProvider provider;
            auto scripts = EvalDescriptorStringOrObject(scanobject, provider);
            for (const auto& script : scripts) {
                std::string inferred = InferDescriptor(script, provider)->ToString();
                needles.emplace(script);
                descriptors.emplace(std::move(script), std::move(inferred));
            }
        }

        // Scan the unspent transaction output set for inputs
        UniValue unspents(UniValue::VARR);
        std::vector<CTxOut> input_txos;
        std::map<COutPoint, Coin> coins;
        g_should_abort_scan = false;
        int64_t count = 0;
        std::unique_ptr<CCoinsViewCursor> pcursor;
        CBlockIndex* tip;
        NodeContext& node = EnsureAnyNodeContext(request.context);
        {
            ChainstateManager& chainman = EnsureChainman(node);
            LOCK(cs_main);
            CChainState& active_chainstate = chainman.ActiveChainstate();
            active_chainstate.ForceFlushStateToDisk();
            pcursor = active_chainstate.CoinsDB().Cursor();
            CHECK_NONFATAL(pcursor);
            tip = active_chainstate.m_chain.Tip();
            CHECK_NONFATAL(tip);
        }
        bool res = FindScriptPubKey(g_scan_progress, g_should_abort_scan, count, pcursor.get(), needles, coins, node.rpc_interruption_point);
        result.pushKV("success", res);
        result.pushKV("txouts", count);
        result.pushKV("height", tip->nHeight);
        result.pushKV("bestblock", tip->GetBlockHash().GetHex());

        for (const auto& it : coins) {
            const COutPoint& outpoint = it.first;
            const Coin& coin = it.second;
            const CTxOut& txo = coin.out;
            input_txos.push_back(txo);
            total_in += txo.nValue;

            UniValue unspent(UniValue::VOBJ);
            unspent.pushKV("txid", outpoint.hash.GetHex());
            unspent.pushKV("vout", (int32_t)outpoint.n);
            unspent.pushKV("scriptPubKey", HexStr(txo.scriptPubKey));
            unspent.pushKV("desc", descriptors[txo.scriptPubKey]);
            unspent.pushKV("amount", ValueFromAmount(txo.nValue));
            unspent.pushKV("height", (int32_t)coin.nHeight);

            unspents.push_back(unspent);
        }
        result.pushKV("unspents", unspents);
        result.pushKV("total_amount", ValueFromAmount(total_in));
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid command");
    }
    return result;
},
    };
}

static RPCHelpMan getblockfilter()
{
    return RPCHelpMan{"getblockfilter",
                "\nRetrieve a BIP 157 content filter for a particular block.\n",
                {
                    {"blockhash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the block"},
                    {"filtertype", RPCArg::Type::STR, RPCArg::Default{"basic"}, "The type name of the filter"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "filter", "the hex-encoded filter data"},
                        {RPCResult::Type::STR_HEX, "header", "the hex-encoded filter header"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\" \"basic\"") +
                    HelpExampleRpc("getblockfilter", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\", \"basic\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    uint256 block_hash = ParseHashV(request.params[0], "blockhash");
    std::string filtertype_name = "basic";
    if (!request.params[1].isNull()) {
        filtertype_name = request.params[1].get_str();
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(filtertype_name, filtertype)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown filtertype");
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        throw JSONRPCError(RPC_MISC_ERROR, "Index is not enabled for filtertype " + filtertype_name);
    }

    const CBlockIndex* block_index;
    bool block_was_connected;
    {
        ChainstateManager& chainman = EnsureAnyChainman(request.context);
        LOCK(cs_main);
        block_index = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!block_index) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        block_was_connected = block_index->IsValid(BLOCK_VALID_SCRIPTS);
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    BlockFilter filter;
    uint256 filter_header;
    if (!index->LookupFilter(block_index, filter) ||
        !index->LookupFilterHeader(block_index, filter_header)) {
        int err_code;
        std::string errmsg = "Filter not found.";

        if (!block_was_connected) {
            err_code = RPC_INVALID_ADDRESS_OR_KEY;
            errmsg += " Block was not connected to active chain.";
        } else if (!index_ready) {
            err_code = RPC_MISC_ERROR;
            errmsg += " Block filters are still in the process of being indexed.";
        } else {
            err_code = RPC_INTERNAL_ERROR;
            errmsg += " This error is unexpected and indicates index corruption.";
        }

        throw JSONRPCError(err_code, errmsg);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("filter", HexStr(filter.GetEncodedFilter()));
    ret.pushKV("header", filter_header.GetHex());
    return ret;
},
    };
}

/**
 * Serialize the UTXO set to a file for loading elsewhere.
 *
 * @see SnapshotMetadata
 */
static RPCHelpMan dumptxoutset()
{
    return RPCHelpMan{
        "dumptxoutset",
        "\nWrite the serialized UTXO set to disk.\n",
        {
            {"path",
                RPCArg::Type::STR,
                RPCArg::Optional::NO,
                /* default_val */ "",
                "path to the output file. If relative, will be prefixed by datadir."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "coins_written", "the number of coins written in the snapshot"},
                    {RPCResult::Type::STR_HEX, "base_hash", "the hash of the base of the snapshot"},
                    {RPCResult::Type::NUM, "base_height", "the height of the base of the snapshot"},
                    {RPCResult::Type::STR, "path", "the absolute path that the snapshot was written to"},
                }
        },
        RPCExamples{
            HelpExampleCli("dumptxoutset", "utxo.dat")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const fs::path path = fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), request.params[0].get_str());
    // Write to a temporary path and then move into `path` on completion
    // to avoid confusion due to an interruption.
    const fs::path temppath = fsbridge::AbsPathJoin(gArgs.GetDataDirNet(), request.params[0].get_str() + ".incomplete");

    if (fs::exists(path)) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            path.string() + " already exists. If you are sure this is what you want, "
            "move it out of the way first");
    }

    FILE* file{fsbridge::fopen(temppath, "wb")};
    CAutoFile afile{file, SER_DISK, CLIENT_VERSION};
    NodeContext& node = EnsureAnyNodeContext(request.context);
    UniValue result = CreateUTXOSnapshot(node, node.chainman->ActiveChainstate(), afile);
    fs::rename(temppath, path);

    result.pushKV("path", path.string());
    return result;
},
    };
}

UniValue CreateUTXOSnapshot(NodeContext& node, CChainState& chainstate, CAutoFile& afile)
{
    std::unique_ptr<CCoinsViewCursor> pcursor;
    CCoinsStats stats{CoinStatsHashType::NONE};
    CBlockIndex* tip;

    {
        // We need to lock cs_main to ensure that the coinsdb isn't written to
        // between (i) flushing coins cache to disk (coinsdb), (ii) getting stats
        // based upon the coinsdb, and (iii) constructing a cursor to the
        // coinsdb for use below this block.
        //
        // Cursors returned by leveldb iterate over snapshots, so the contents
        // of the pcursor will not be affected by simultaneous writes during
        // use below this block.
        //
        // See discussion here:
        //   https://github.com/bitcoin/bitcoin/pull/15606#discussion_r274479369
        //
        LOCK(::cs_main);

        chainstate.ForceFlushStateToDisk();

        if (!GetUTXOStats(&chainstate.CoinsDB(), chainstate.m_blockman, stats, node.rpc_interruption_point)) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Unable to read UTXO set");
        }

        pcursor = chainstate.CoinsDB().Cursor();
        tip = chainstate.m_blockman.LookupBlockIndex(stats.hashBlock);
        CHECK_NONFATAL(tip);
    }

    SnapshotMetadata metadata{tip->GetBlockHash(), stats.coins_count, tip->nChainTx};

    afile << metadata;

    COutPoint key;
    Coin coin;
    unsigned int iter{0};

    while (pcursor->Valid()) {
        if (iter % 5000 == 0) node.rpc_interruption_point();
        ++iter;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            afile << key;
            afile << coin;
        }

        pcursor->Next();
    }

    afile.fclose();

    UniValue result(UniValue::VOBJ);
    result.pushKV("coins_written", stats.coins_count);
    result.pushKV("base_hash", tip->GetBlockHash().ToString());
    result.pushKV("base_height", tip->nHeight);

    return result;
}

static RPCHelpMan arc20name()
{
    return RPCHelpMan{"arc20name",
                "\nReturns the name of the token\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                },
                RPCResult{
                    RPCResult::Type::STR, "name", "The name of the token"},
                RPCExamples{
                    HelpExampleCli("arc20name", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
            + HelpExampleRpc("arc20name", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Set contract address
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());

    // Get name
    std::string result;
    if(!token.name(result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get token name");

    return result;
},
    };
}

static RPCHelpMan arc20symbol()
{
    return RPCHelpMan{"arc20symbol",
                "\nReturns the symbol of the token\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                },
                RPCResult{
                    RPCResult::Type::STR, "symbol", "The symbol of the token"},
                RPCExamples{
                    HelpExampleCli("arc20symbol", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
            + HelpExampleRpc("arc20symbol", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Set contract address
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());

    // Get symbol
    std::string result;
    if(!token.symbol(result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get symbol");

    return result;
},
    };
}

static RPCHelpMan arc20totalsupply()
{
    return RPCHelpMan{"arc20totalsupply",
                "\nReturns the total supply of the token\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                },
                RPCResult{
                    RPCResult::Type::STR, "totalSupply", "The total supply of the token"},
                RPCExamples{
                    HelpExampleCli("arc20totalsupply", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
            + HelpExampleRpc("arc20totalsupply", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Set contract address
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());

    // Get total supply
    std::string result;
    if(!token.totalSupply(result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get total supply");

    // Get decimals
    uint32_t decimals;
    if(!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Check value
    dev::s256 value(result);
    if(value < 0)
        throw JSONRPCError(RPC_MISC_ERROR, "Invalid total supply, value must be positive");

    return FormatToken(decimals, value);
},
    };
}

static RPCHelpMan arc20decimals()
{
    return RPCHelpMan{"arc20decimals",
                "\nReturns the number of decimals of the token\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                },
                RPCResult{
                    RPCResult::Type::NUM, "decimals", "The number of decimals of the token"},
                RPCExamples{
                    HelpExampleCli("arc20decimals", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
            + HelpExampleRpc("arc20decimals", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Set contract address
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());
    uint32_t result;

    // Get decimals
    if(!token.decimals(result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    return (int)result;
},
    };
}

static RPCHelpMan arc20balanceof()
{
    return RPCHelpMan{"arc20balanceof",
                "\nReturns the token balance for address\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO,  "The qtum address to check token balance"},
                },
                RPCResult{
                    RPCResult::Type::STR, "balance", "The token balance of the chosen address"},
                RPCExamples{
                    HelpExampleCli("arc20balanceof", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"")
            + HelpExampleRpc("arc20balanceof", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Get parameters
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());
    std::string sender = request.params[1].get_str();
    token.setSender(sender);

    // Get balance of address
    std::string result;
    if(!token.balanceOf(result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get balance");

    // Get decimals
    uint32_t decimals;
    if(!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Check value
    dev::s256 value(result);
    if(value < 0)
        throw JSONRPCError(RPC_MISC_ERROR, "Invalid balance, vout must be positive");

    return FormatToken(decimals, value);
},
    };
}

static RPCHelpMan arc20allowance()
{
    return RPCHelpMan{"arc20allowance",
                "\nReturns remaining tokens allowed to spend for an address\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address"},
                    {"addressfrom", RPCArg::Type::STR, RPCArg::Optional::NO,  "The qtum address of the account owning tokens"},
                    {"addressto", RPCArg::Type::STR, RPCArg::Optional::NO,  "The qtum address of the account able to transfer the tokens"},
                },
                RPCResult{
                    RPCResult::Type::STR, "allowance", "Amount of remaining tokens allowed to spent"},
                RPCExamples{
                    HelpExampleCli("arc20allowance", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"QM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
            + HelpExampleRpc("arc20allowance", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" \"QM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Set contract address
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());

    // Get total supply
    std::string result;
    if(!token.allowance(request.params[1].get_str(), request.params[2].get_str(), result))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get allowance");

    // Get decimals
    uint32_t decimals;
    if(!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Check value
    dev::s256 value(result);
    if(value < 0)
        throw JSONRPCError(RPC_MISC_ERROR, "Invalid allowance, value must be positive");

    return FormatToken(decimals, value);
},
    };
}

static RPCHelpMan arc20listtransactions()
{
    return RPCHelpMan{"arc20listtransactions",
                "\nReturns transactions history for a specific address.\n",
                {
                    {"contractaddress", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The contract address."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO,  "The qtum address to get history for."},
                    {"fromblock", RPCArg::Type::NUM, RPCArg::Default{0}, "The number of the earliest block."},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{6}, "Minimal number of confirmations."},
                },
               RPCResult{
            RPCResult::Type::ARR, "", "",
                {
                    {RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR, "receiver", "The receiver qtum address"},
                            {RPCResult::Type::STR, "sender", "The sender qtum address"},
                            {RPCResult::Type::STR_AMOUNT, "amount", "The transferred token amount"},
                            {RPCResult::Type::NUM, "confirmations", "The number of confirmations of the most recent transaction included"},
                            {RPCResult::Type::STR_HEX, "blockHash", "The block hash"},
                            {RPCResult::Type::NUM, "blockNumber", "The block number"},
                            {RPCResult::Type::NUM_TIME, "blocktime", "The block time expressed in " + UNIX_EPOCH_TIME + "."},
                            {RPCResult::Type::STR_HEX, "transactionHash", "The transaction hash"},
                        }
                    }}
                },
                RPCExamples{
                    HelpExampleCli("arc20listtransactions", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"")
            + HelpExampleCli("arc20listtransactions", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 0 6")
            + HelpExampleRpc("arc20listtransactions", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\"")
            + HelpExampleRpc("arc20listtransactions", "\"eb23c0b3e6042821da281a2e2364feb22dd543e3\" \"QX1GkJdye9WoUnrE2v6ZQhQ72EUVDtGXQX\" 0 6")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Get parameters
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CallToken token(chainman);
    token.setAddress(request.params[0].get_str());
    std::string sender = request.params[1].get_str();
    token.setSender(sender);
    int64_t fromBlock = 0;
    int64_t minconf = 6;
    if(!request.params[2].isNull())
        fromBlock = request.params[2].get_int64();
    if(!request.params[3].isNull())
        minconf = request.params[3].get_int64();

    // Get transaction events
    LOCK(cs_main);
    std::vector<TokenEvent> result;
    CChain& active_chain = chainman.ActiveChain();
    int64_t toBlock = active_chain.Height();
    if(!token.transferEvents(result, fromBlock, toBlock, minconf))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get transfer events");
    if(!token.burnEvents(result, fromBlock, toBlock, minconf))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get burn events");

    // Get decimals
    uint32_t decimals;
    if(!token.decimals(decimals))
        throw JSONRPCError(RPC_MISC_ERROR, "Fail to get decimals");

    // Create transaction list
    UniValue res(UniValue::VARR);
    for(const auto& event : result){
        UniValue obj(UniValue::VOBJ);

        obj.pushKV("receiver", event.receiver);
        obj.pushKV("sender", event.sender);
        dev::s256 v = uintTou256(event.value);
        dev::s256 value;
        if(event.sender == event.receiver)
            value = 0;
        else if(event.receiver == sender)
            value = v;
        else
            value = -v;
        obj.pushKV("amount", FormatToken(decimals, value));
        int confirms = toBlock - event.blockNumber + 1;
        obj.pushKV("confirmations", confirms);
        obj.pushKV("blockHash", event.blockHash.GetHex());
        obj.pushKV("blockNumber", event.blockNumber);
        obj.pushKV("blocktime", active_chain[event.blockNumber]->GetBlockTime());
        obj.pushKV("transactionHash", event.transactionHash.GetHex());
        res.push_back(obj);
    }

    return res;
},
    };
}

void RegisterBlockchainRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "blockchain",         &getblockchaininfo,                  },
    { "blockchain",         &getchaintxstats,                    },
    { "blockchain",         &getblockstats,                      },
    { "blockchain",         &getbestblockhash,                   },
    { "blockchain",         &getblockcount,                      },
    { "blockchain",         &getblock,                           },
    { "blockchain",         &getblockhash,                       },
    { "blockchain",         &getblockheader,                     },
    { "blockchain",         &getchaintips,                       },
    { "blockchain",         &getdifficulty,                      },
    { "blockchain",         &getmempoolancestors,                },
    { "blockchain",         &getmempooldescendants,              },
    { "blockchain",         &getmempoolentry,                    },
    { "blockchain",         &getmempoolinfo,                     },
    { "blockchain",         &getrawmempool,                      },
    { "blockchain",         &gettxout,                           },
    { "blockchain",         &gettxoutsetinfo,                    },
    { "blockchain",         &pruneblockchain,                    },
    { "blockchain",         &savemempool,                        },
    { "blockchain",         &verifychain,                        },
    { "blockchain",         &getaccountinfo,                     },
    { "blockchain",         &getstorage,                         },

    { "blockchain",         &preciousblock,                      },
    { "blockchain",         &scantxoutset,                       },
    { "blockchain",         &getblockfilter,                     },

    { "blockchain",         &callcontract,                       },

    { "blockchain",         &arc20name,                          },
    { "blockchain",         &arc20symbol,                        },
    { "blockchain",         &arc20totalsupply,                   },
    { "blockchain",         &arc20decimals,                      },
    { "blockchain",         &arc20balanceof,                     },
    { "blockchain",         &arc20allowance,                     },
    { "blockchain",         &arc20listtransactions,              },

    { "blockchain",         &listcontracts,                      },
    { "blockchain",         &gettransactionreceipt,              },
    { "blockchain",         &searchlogs,                         },

    { "blockchain",         &waitforlogs,                        },
    { "blockchain",         &getestimatedannualroi,              },

    /* Not shown in help */
    { "hidden",              &invalidateblock,                   },
    { "hidden",              &reconsiderblock,                   },
    { "hidden",              &waitfornewblock,                   },
    { "hidden",              &waitforblock,                      },
    { "hidden",              &waitforblockheight,                },
    { "hidden",              &syncwithvalidationinterfacequeue,  },
    { "hidden",              &dumptxoutset,                      },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
