/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Currently maintained by:
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

extern "C" {
#include <qemu-common.h>
#include <exec-all.h>
}

#include "CacheSim.h"

#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/S2EExecutionState.h>
#include <s2e/Utils.h>
#include <s2e/Database.h>

#include <llvm/Support/TimeValue.h>

#include <iostream>
#include <stdlib.h>
#include <stdio.h>

#define CACHESIM_LOG_SIZE 4096

namespace s2e {
namespace plugins {

using namespace std;
using namespace klee;

/** Returns the floor form of binary logarithm for a 32 bit integer.
    (unsigned) -1 is returned if n is 0. */
uint64_t floorLog2(uint64_t n) {
    int pos = 0;
    if (n >= 1<<16) { n >>= 16; pos += 16; }
    if (n >= 1<< 8) { n >>=  8; pos +=  8; }
    if (n >= 1<< 4) { n >>=  4; pos +=  4; }
    if (n >= 1<< 2) { n >>=  2; pos +=  2; }
    if (n >= 1<< 1) {           pos +=  1; }
    return ((n == 0) ? ((uint64_t)-1) : pos);
}

/* Model of n-way accosiative write-through LRU cache */
class Cache {
protected:
    uint64_t m_size;
    uint64_t m_associativity;
    uint64_t m_lineSize;

    uint64_t m_indexShift; // log2(m_lineSize)
    uint64_t m_indexMask;  // 1 - setsCount

    uint64_t m_tagShift;   // m_indexShift + log2(setsCount)

    std::vector<uint64_t> m_lines;

    std::string m_name;
    uint8_t m_cacheId;

    Cache* m_upperCache;

public:
    uint64_t getSize() const {
        return m_size;
    }

    uint64_t getAssociativity() const {
        return m_associativity;
    }

    uint64_t getLineSize() const {
        return m_lineSize;
    }

    uint8_t getId() const {
        return m_cacheId;
    }

    void setId(uint8_t id) {
        m_cacheId = id;
    }


    Cache(const std::string& name,
          uint64_t size, uint64_t associativity,
          uint64_t lineSize, uint64_t cost = 1, Cache* upperCache = NULL)
        : m_size(size), m_associativity(associativity), m_lineSize(lineSize),
          m_name(name), m_upperCache(upperCache)
    {
        assert(size && associativity && lineSize);

        assert(uint64_t(1LL<<floorLog2(associativity)) == associativity);
        assert(uint64_t(1LL<<floorLog2(lineSize)) == lineSize);

        uint64_t setsCount = (size / lineSize) / associativity;
        assert(setsCount && uint64_t(1LL << floorLog2(setsCount)) == setsCount);

        m_indexShift = floorLog2(m_lineSize);
        m_indexMask = setsCount-1;

        m_tagShift = floorLog2(setsCount) + m_indexShift;

        m_lines.resize(setsCount * associativity, (uint64_t) -1);
    }

    const std::string& getName() const { return m_name; }

    Cache* getUpperCache() { return m_upperCache; }
    void setUpperCache(Cache* cache) { m_upperCache = cache; }

    /** Models a cache access. A misCount is an array for miss counts (will be
        passed to the upper caches), misCountSize is its size. Array
        must be zero-initialized. */
    void access(uint64_t address, uint64_t size,
            bool isWrite, unsigned* misCount, unsigned misCountSize)
    {

        uint64_t s1 = address >> m_indexShift;
        uint64_t s2 = (address+size-1) >> m_indexShift;

        if(s1 != s2) {
            /* Cache access spawns multiple lines */
            uint64_t size1 = m_lineSize - (address & (m_lineSize - 1));
            access(address, size1, isWrite, misCount, misCountSize);
            access((address & ~(m_lineSize-1)) + m_lineSize, size-size1,
                                   isWrite, misCount, misCountSize);
            return;
        }

        uint64_t set = s1 & m_indexMask;
        uint64_t l = set * m_associativity;
        uint64_t tag = address >> m_tagShift;

        for(unsigned i = 0; i < m_associativity; ++i) {
            if(m_lines[l + i] == tag) {
                /* Cache hit. Move line to MRU. */
                for(unsigned j = i; j > 0; --j)
                    m_lines[l + j] = m_lines[l + j - 1];
                m_lines[l] = tag;
                return;
            }
        }

        //g_s2e->getDebugStream() << "Miss at 0x" << std::hex << address << '\n';
        /* Cache miss. Install new tag as MRU */
        misCount[0] += 1;
        for(unsigned j = m_associativity-1; j > 0; --j)
            m_lines[l + j] = m_lines[l + j - 1];
        m_lines[l] = tag;

        if(m_upperCache) {
            assert(misCountSize > 1);
            m_upperCache->access(address, size, isWrite,
                                 misCount+1, misCountSize-1);
        }
    }
};

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

CacheSimState::CacheSimState()
{
    m_i1_length = 0;
    m_d1_length = 0;
    m_i1 = NULL;
    m_d1 = NULL;
}

CacheSimState::CacheSimState(S2EExecutionState *s, Plugin *p)
{
    CacheSim *csp = dynamic_cast<CacheSim*>(p);
    S2E *s2e = csp->s2e();

    const std::string &cfgKey = p->getConfigKey();
    ConfigFile* conf = s2e->getConfig();

    if (csp->m_d1_connection.connected()) {
        csp->m_d1_connection.disconnect();
    }

    if (csp->m_i1_connection.connected()) {
        csp->m_i1_connection.disconnect();
    }


    //Initialize the cache configuration
    vector<string> caches = conf->getListKeys(cfgKey + ".caches");
    foreach(const string& cacheName, caches) {
        string key = cfgKey + ".caches." + cacheName;
        Cache* cache = new Cache(cacheName,
                                 conf->getInt(key + ".size"),
                                 conf->getInt(key + ".associativity"),
                                 conf->getInt(key + ".lineSize"));

        m_caches.insert(make_pair(cacheName, cache));
    }

    foreach(const CachesMap::value_type& ci, m_caches) {
        string key = cfgKey + ".caches." + ci.first + ".upper";
        if(conf->hasKey(key))
            ci.second->setUpperCache(getCache(conf->getString(key)));
    }

    if(conf->hasKey(cfgKey + ".i1"))
        m_i1 = getCache(conf->getString(cfgKey + ".i1"));

    if(conf->hasKey(cfgKey + ".d1"))
        m_d1 = getCache(conf->getString(cfgKey + ".d1"));

    m_i1_length = 0;
    m_d1_length = 0;


    s2e->getMessagesStream() << "Instruction cache hierarchy:";
    for(Cache* c = m_i1; c != NULL; c = c->getUpperCache()) {
        m_i1_length += 1;
        s2e->getMessagesStream() << " -> " << c->getName();
    }
    s2e->getMessagesStream() << " -> memory" << '\n';

    s2e->getMessagesStream() << "Data cache hierarchy:";
    for(Cache* c = m_d1; c != NULL; c = c->getUpperCache()) {
        m_d1_length += 1;
        s2e->getMessagesStream() << " -> " << c->getName();
    }
    s2e->getMessagesStream() << " -> memory" << '\n';


    if (csp->m_execDetector && csp->m_startOnModuleLoad){
        s2e->getDebugStream()  << "Connecting to onModuleTranslateBlockStart" << '\n';
        csp->m_ModuleConnection = csp->m_execDetector->onModuleTranslateBlockStart.connect(
                sigc::mem_fun(*csp, &CacheSim::onModuleTranslateBlockStart));

    }else {
        if(m_d1) {
            s2e->getDebugStream()  << "CacheSim: connecting to onDataMemoryAccess" << '\n';
            s2e->getCorePlugin()->onDataMemoryAccess.connect(
                sigc::mem_fun(*csp, &CacheSim::onDataMemoryAccess));
        }

        if(m_i1) {
            s2e->getDebugStream()  << "CacheSim: connecting to onTranslateBlockStart" << '\n';
            s2e->getCorePlugin()->onTranslateBlockStart.connect(
             sigc::mem_fun(*csp, &CacheSim::onTranslateBlockStart));
        }
    }

    s2e->getCorePlugin()->onTimer.connect(
        sigc::mem_fun(*csp, &CacheSim::onTimer)
    );

}

CacheSimState::~CacheSimState()
{
    foreach(const CachesMap::value_type& ci, m_caches)
        delete ci.second;
}

PluginState *CacheSimState::factory(Plugin *p, S2EExecutionState *s)
{
    p->s2e()->getDebugStream() << "Creating initial CacheSimState" << '\n';
    CacheSimState *ret = new CacheSimState(s, p);
    return ret;

}

PluginState *CacheSimState::clone() const
{
    CacheSimState *ret = new CacheSimState(*this);

    //Clone the caches first
    CachesMap::iterator newCaches;
    for (newCaches = ret->m_caches.begin(); newCaches != ret->m_caches.end(); ++newCaches) {
        (*newCaches).second = new Cache(*(*newCaches).second);
    }

    //Update the upper cache mappings
    CachesMap::const_iterator oldCaches;
    for (oldCaches = m_caches.begin(); oldCaches != m_caches.end(); ++oldCaches) {
        Cache *u;
        if (!(u = (*oldCaches).second->getUpperCache())) {
            continue;
        }

        CachesMap::iterator newCache = ret->m_caches.find(u->getName());
        assert(newCache != ret->m_caches.end());
        (*newCache).second->setUpperCache((*newCache).second);
    }

    ret->m_d1 = ret->m_caches[m_d1->getName()];
    assert(ret->m_d1);

    ret->m_i1 = ret->m_caches[m_i1->getName()];
    assert(ret->m_i1);

    return ret;
}

inline Cache* CacheSimState::getCache(const std::string& name)
{
    CachesMap::iterator it = m_caches.find(name);
    if(it == m_caches.end()) {
        cerr << "ERROR: cache " << name << " undefined" << endl;
        exit(1);
    }
    return it->second;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////


S2E_DEFINE_PLUGIN(CacheSim, "Cache simulator", "",);

CacheSim::~CacheSim()
{
    flushLogEntries();

}


void CacheSim::initialize()
{
    ConfigFile* conf = s2e()->getConfig();

    m_execDetector = (ModuleExecutionDetector*)s2e()->getPlugin("ModuleExecutionDetector");
    m_Tracer = (ExecutionTracer*)s2e()->getPlugin("ExecutionTracer");

    if (!m_execDetector) {
        s2e()->getMessagesStream() << "ModuleExecutionDetector not found, will profile the whole system" << '\n';
    }

    //Report misses form the entire system
    m_reportWholeSystem = conf->getBool(getConfigKey() + ".reportWholeSystem");

    //Option to write only those instructions that cause misses
    m_reportZeroMisses = conf->getBool(getConfigKey() + ".reportZeroMisses");

    //Profile only for the selected modules
    m_profileModulesOnly = conf->getBool(getConfigKey() + ".profileModulesOnly");

    //Write to the binary log instead of the sql db.
    m_useBinaryLogFile = conf->getBool(getConfigKey() + ".useBinaryLogFile");

    //Start cache profiling when the first configured module is loaded
    m_startOnModuleLoad = conf->getBool(getConfigKey() + ".startOnModuleLoad");

    //Determines whether to address the cache physically of virtually
    m_physAddress = conf->getBool(getConfigKey() + ".physicalAddressing");

    m_cacheStructureWrittenToLog = false;
    if (m_useBinaryLogFile && !m_Tracer) {
        s2e()->getWarningsStream() << "ExecutionTracer is required when useBinaryLogFile is set!" << '\n';
        exit(-1);
    }

    ////////////////////
    //XXX: trick to force the initialization of the cache upon first memory access.
    m_d1_connection = s2e()->getCorePlugin()->onDataMemoryAccess.connect(
         sigc::mem_fun(*this, &CacheSim::onDataMemoryAccess));

    m_i1_connection = s2e()->getCorePlugin()->onTranslateBlockStart.connect(
         sigc::mem_fun(*this, &CacheSim::onTranslateBlockStart));

    ////////////////////
    const char *query = "create table CacheSim("
          "'timestamp' unsigned big int, "
          "'pc' unsigned big int, "
          "'address' unsigned bit int, "
          "'size' unsigned int, "
          "'isWrite' boolean, "
          "'isCode' boolean, "
          "'cacheName' varchar(30), "
          "'missCount' unsigned int"
          "); create index if not exists CacheSimIdx on CacheSim (pc);";

    bool ok = s2e()->getDb()->executeQuery(query);
    assert(ok && "create table failed");

    m_cacheLog.reserve(CACHESIM_LOG_SIZE);


}

void CacheSim::writeCacheDescriptionToLog(S2EExecutionState *state)
{
    if (!m_useBinaryLogFile || m_cacheStructureWrittenToLog) {
        return;
    }

    DECLARE_PLUGINSTATE(CacheSimState, state);

    //Output the names of the caches
    uint8_t cacheId = 0;
    foreach2(it, plgState->m_caches.begin(), plgState->m_caches.end()) {
        (*it).second->setId(cacheId++);
        uint32_t retsize;
        ExecutionTraceCacheSimName *n = ExecutionTraceCacheSimName::allocate((*it).second->getId(),
                                                                             (*it).first, &retsize);

        m_Tracer->writeData(state, n, retsize, TRACE_CACHESIM);
        ExecutionTraceCacheSimName::deallocate(n);
        m_Tracer->flush();
    }

    //Output the configuration of the caches
    foreach2(it, plgState->m_caches.begin(), plgState->m_caches.end()) {
        ExecutionTraceCacheSimParams p;
        p.type = CACHE_PARAMS;
        p.size = (*it).second->getSize();
        p.associativity = (*it).second->getAssociativity();
        p.lineSize = (*it).second->getLineSize();

        if ((*it).second->getUpperCache()) {
            p.upperCacheId = (*it).second->getUpperCache()->getId();
        }else {
            p.upperCacheId = (unsigned)-1;
        }

        p.cacheId = (*it).second->getId();
        m_Tracer->writeData(state, &p, sizeof(p), TRACE_CACHESIM);
        m_Tracer->flush();
    }

    m_Tracer->flush();
    m_cacheStructureWrittenToLog = true;
}

//Connect the tracing when the first module is loaded
void CacheSim::onModuleTranslateBlockStart(
    ExecutionSignal* signal,
    S2EExecutionState *state,
    const ModuleDescriptor &desc,
    TranslationBlock *tb, uint64_t pc)
{

    DECLARE_PLUGINSTATE(CacheSimState, state);

    s2e()->getDebugStream() << "Module translation CacheSim " << desc.Name << "  " <<
        pc <<'\n';

    if(plgState->m_d1)
        s2e()->getCorePlugin()->onDataMemoryAccess.connect(
            sigc::mem_fun(*this, &CacheSim::onDataMemoryAccess));

    if(plgState->m_i1)
        s2e()->getCorePlugin()->onTranslateBlockStart.connect(
            sigc::mem_fun(*this, &CacheSim::onTranslateBlockStart));

    //We connected ourselves, do not need to monitor modules anymore.
    s2e()->getDebugStream()  << "Disconnecting module translation cache sim" << '\n';
    m_ModuleConnection.disconnect();
}

//Periodically flush the cache
void CacheSim::onTimer()
{
    flushLogEntries();
}

void CacheSim::flushLogEntries()
{
    if (m_useBinaryLogFile) {
        return;
    }

    char query[512];
    bool ok = s2e()->getDb()->executeQuery("begin transaction;");
    assert(ok && "Can not execute database query");
    foreach(const CacheLogEntry& ce, m_cacheLog) {
        snprintf(query, sizeof(query),
             "insert into CacheSim values(%"PRIu64",%"PRIu64",%"PRIu64",%u,%u,%u,'%s',%u);",
             ce.timestamp, ce.pc, ce.address, ce.size,
             ce.isWrite, ce.isCode,
             ce.cacheName, ce.missCount);
        ok = s2e()->getDb()->executeQuery(query);
        assert(ok && "Can not execute database query");
    }
    ok = s2e()->getDb()->executeQuery("end transaction;");
    assert(ok && "Can not execute database query");
    m_cacheLog.resize(0);
}

bool CacheSim::profileAccess(S2EExecutionState *state) const
{
    //Check whether to profile only known modules
    if (!m_reportWholeSystem) {
        if (m_execDetector && m_profileModulesOnly) {
            if (!m_execDetector->getCurrentDescriptor(state)) {
                return false;
            }
        }
    }

    return true;
}

bool CacheSim::reportAccess(S2EExecutionState *state) const
{
    bool doLog = m_reportWholeSystem;

    if (!m_reportWholeSystem) {
        if (m_execDetector) {
            return (m_execDetector->getCurrentDescriptor(state) != NULL);
        }else {
            return false;
        }
    }

    return doLog;
}

void CacheSim::onMemoryAccess(S2EExecutionState *state,
                              uint64_t address, unsigned size,
                              bool isWrite, bool isIO, bool isCode)
{
    if(isIO) /* this is only an estimation - should look at registers! */
        return;

    DECLARE_PLUGINSTATE(CacheSimState, state);

    if (!profileAccess(state)) {
        return;
    }


    Cache* cache = isCode ? plgState->m_i1 : plgState->m_d1;
    if(!cache)
        return;

    //Done only on the first invocation
    writeCacheDescriptionToLog(state);

    unsigned missCountLength = isCode ? plgState->m_i1_length : plgState->m_d1_length;
    unsigned missCount[missCountLength];
    memset(missCount, 0, sizeof(missCount));
    cache->access(address, size, isWrite, missCount, missCountLength);

    //Decide whether to log the access in the database
    if (!reportAccess(state)) {
        return;
    }

    unsigned i = 0;
    for(Cache* c = cache; c != NULL; c = c->getUpperCache(), ++i) {
        if(m_cacheLog.size() == CACHESIM_LOG_SIZE)
            flushLogEntries();

       // std::cout << state->getPc() << " "  << c->getName() << ": " << missCount[i] << '\n';

        if (m_reportZeroMisses || missCount[i]) {
            if (m_useBinaryLogFile) {
                ExecutionTraceCacheSimEntry e;
                e.type = CACHE_ENTRY;
                e.cacheId = c->getId();
                e.pc = state->getPc();
                e.address = address;
                e.size = size;
                e.isWrite = isWrite;
                e.isCode = isCode;
                e.missCount = missCount[i];
                m_Tracer->writeData(state, &e, sizeof(e), TRACE_CACHESIM);
            }else {
                m_cacheLog.resize(m_cacheLog.size()+1);
                CacheLogEntry& ce = m_cacheLog.back();
                ce.timestamp = llvm::sys::TimeValue::now().usec();
                ce.pc = state->getPc();
                ce.address = address;
                ce.size = size;
                ce.isWrite = isWrite;
                ce.isCode = false;
                ce.cacheName = c->getName().c_str();
                ce.missCount = missCount[i];
            }
        }

        if(missCount[i] == 0)
            break;
    }
}

void CacheSim::onDataMemoryAccess(S2EExecutionState *state,
                              klee::ref<klee::Expr> address,
                              klee::ref<klee::Expr> hostAddress,
                              klee::ref<klee::Expr> value,
                              bool isWrite, bool isIO)
{
    if(!isa<ConstantExpr>(hostAddress)) {
        s2e()->getWarningsStream()
                << "Warning: CacheSim do not support symbolic addresses"
                << '\n';
        return;
    }

    uint64_t constAddress;
    unsigned size = Expr::getMinBytesForWidth(value->getWidth());

    if (m_physAddress) {
        constAddress = cast<ConstantExpr>(hostAddress)->getZExtValue(64);
  //      s2e()->getDebugStream() << "acc pc=" << std::hex << state->getPc() << " ha=" << constAddress << '\n';
    }else {
        constAddress = cast<ConstantExpr>(address)->getZExtValue(64);
    }

    onMemoryAccess(state, constAddress, size, isWrite, isIO, false);
}

void CacheSim::onExecuteBlockStart(S2EExecutionState *state, uint64_t pc,
                                   TranslationBlock *tb, uint64_t hostAddress)
{
//    s2e()->getDebugStream() << "exec pc=" << std::hex << pc << " ha=" << hostAddress << '\n';
    onMemoryAccess(state, m_physAddress ? hostAddress : pc, tb->size, false, false, true);
}

void CacheSim::onTranslateBlockStart(ExecutionSignal *signal,
                                     S2EExecutionState *state,
                                     TranslationBlock *tb,
                                     uint64_t)
{
    uint64_t newPc;

    if (m_physAddress) {
        newPc = state->getHostAddress(tb->pc);
        //s2e()->getDebugStream() << "tb pc=" << std::hex << tb->pc << " ha=" << newPc << '\n';
    }else {
        newPc = tb->pc;
    }

    signal->connect(sigc::bind(
            sigc::mem_fun(*this, &CacheSim::onExecuteBlockStart), tb, newPc));
}


} // namespace plugins
} // namespace s2e
