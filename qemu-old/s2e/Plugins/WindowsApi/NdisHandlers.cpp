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
#include <cpu-all.h>
#include <exec-all.h>
}

#include "NdisHandlers.h"
#include "Ndis.h"
#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>

#include <s2e/Plugins/WindowsInterceptor/WindowsImage.h>
#include <s2e/Plugins/MemoryChecker.h>
#include <klee/Solver.h>

#include <iostream>
#include <sstream>

using namespace s2e::windows;

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(NdisHandlers, "Basic collection of NDIS API functions.", "NdisHandlers",
                  "FunctionMonitor", "Interceptor", "ModuleExecutionDetector", "StateManager", "SymbolicHardware");


//This maps exported NDIS functions to their handlers
const NdisHandlers::AnnotationsArray NdisHandlers::s_handlers[] = {

    DECLARE_EP_STRUC(NdisHandlers, NdisAllocateBuffer),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocateBufferPool),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocateMemory),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocateMemoryWithTag),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocateMemoryWithTagPriority),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocatePacket),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocatePacketPool),
    DECLARE_EP_STRUC(NdisHandlers, NdisAllocatePacketPoolEx),

    DECLARE_EP_STRUC(NdisHandlers, NdisCloseConfiguration),
    DECLARE_EP_STRUC(NdisHandlers, NdisFreeMemory),
    DECLARE_EP_STRUC(NdisHandlers, NdisFreePacket),

    DECLARE_EP_STRUC(NdisHandlers, NdisMAllocateMapRegisters),
    DECLARE_EP_STRUC(NdisHandlers, NdisMAllocateSharedMemory),

    DECLARE_EP_STRUC(NdisHandlers, NdisMFreeSharedMemory),
    DECLARE_EP_STRUC(NdisHandlers, NdisMInitializeTimer),
    DECLARE_EP_STRUC(NdisHandlers, NdisMMapIoSpace),
    DECLARE_EP_STRUC(NdisHandlers, NdisMQueryAdapterInstanceName),
    DECLARE_EP_STRUC(NdisHandlers, NdisMQueryAdapterResources),
    DECLARE_EP_STRUC(NdisHandlers, NdisMRegisterAdapterShutdownHandler),
    DECLARE_EP_STRUC(NdisHandlers, NdisMRegisterInterrupt),
    DECLARE_EP_STRUC(NdisHandlers, NdisMRegisterIoPortRange),
    DECLARE_EP_STRUC(NdisHandlers, NdisMRegisterMiniport),
    DECLARE_EP_STRUC(NdisHandlers, NdisMSetAttributes),
    DECLARE_EP_STRUC(NdisHandlers, NdisMSetAttributesEx),

    DECLARE_EP_STRUC(NdisHandlers, NdisOpenAdapter),
    DECLARE_EP_STRUC(NdisHandlers, NdisOpenConfiguration),
    DECLARE_EP_STRUC(NdisHandlers, NdisQueryAdapterInstanceName),
    DECLARE_EP_STRUC(NdisHandlers, NdisQueryPendingIOCount),


    DECLARE_EP_STRUC(NdisHandlers, NdisReadConfiguration),
    DECLARE_EP_STRUC(NdisHandlers, NdisReadNetworkAddress),
    DECLARE_EP_STRUC(NdisHandlers, NdisReadPciSlotInformation),
    DECLARE_EP_STRUC(NdisHandlers, NdisRegisterProtocol),
    DECLARE_EP_STRUC(NdisHandlers, NdisSetTimer),
    DECLARE_EP_STRUC(NdisHandlers, NdisWriteErrorLogEntry),
    DECLARE_EP_STRUC(NdisHandlers, NdisWritePciSlotInformation),
};

const char *NdisHandlers::s_ignoredFunctionsList[] = {
    "NdisCancelSendPackets",
    "NdisCloseAdapter",
    "NdisCopyFromPacketToPacket",
    "NdisGeneratePartialCancelId",
    "NdisInitializeEvent",
    "NdisReturnPackets",
    "NdisSetEvent",
    "NdisUnchainBufferAtFront",
    "NdisWaitEvent",

    //XXX: Revoke rights for these
    "NdisDeregisterProtocol",
    "NdisFreeBufferPool",
    "NdisFreePacketPool",


    NULL
};

//XXX: Implement these
//NdisQueryAdapterInstanceName, NdisQueryPendingIOCount
//NdisRequest

const SymbolDescriptor NdisHandlers::s_exportedVariablesList[] = {
    {"", 0}
};

const NdisHandlers::AnnotationsMap NdisHandlers::s_handlersMap =
        NdisHandlers::initializeHandlerMap();

const NdisHandlers::StringSet NdisHandlers::s_ignoredFunctions =
        NdisHandlers::initializeIgnoredFunctionSet();

const SymbolDescriptors NdisHandlers::s_exportedVariables =
        NdisHandlers::initializeExportedVariables();


void NdisHandlers::initialize()
{
    ConfigFile *cfg = s2e()->getConfig();

    WindowsApi::initialize();

    m_hw = static_cast<SymbolicHardware*>(s2e()->getPlugin("SymbolicHardware"));


    bool ok;
    m_devDesc = NULL;
    m_hwId = cfg->getString(getConfigKey() + ".hwId", "", &ok);
    if (!ok) {
        s2e()->getWarningsStream() << "NDISHANDLERS: You did not configure any symbolic hardware id" << '\n';
    }else {
        m_devDesc = m_hw->findDevice(m_hwId);
        if (!m_devDesc) {
            s2e()->getWarningsStream() << "NDISHANDLERS: The specified hardware device id is invalid " << m_hwId << '\n';
            exit(-1);
        }
    }

    //Checking the keywords for NdisReadConfiguration whose result will not be replaced with symbolic values
    ConfigFile::string_list ign = cfg->getStringList(getConfigKey() + ".ignoreKeywords");
    m_ignoreKeywords.insert(ign.begin(), ign.end());

    //The multiplicative interval factor slows down the frequency of the registered timers
    //via NdisSetTimer.
    m_timerIntervalFactor = cfg->getInt(getConfigKey() + ".timerIntervalFactor", 1, &ok);

    //Read the hard-coded mac address, in case we do not want it to be symbolic
    ConfigFile::integer_list mac = cfg->getIntegerList(getConfigKey() + ".networkAddress",
                                                       ConfigFile::integer_list(), &ok);
    if (ok && mac.size() > 0) {
        foreach2(it, mac.begin(), mac.end()) {
            m_networkAddress.push_back(*it);
        }
    }

    //What device type do we want to force?
    //This is only for overapproximate consistency
    m_forceAdapterType = cfg->getInt(getConfigKey() + ".forceAdapterType", InterfaceTypeUndefined, &ok);

    //For debugging: generate a crash dump when the driver is loaded
    m_generateDumpOnLoad = cfg->getBool(getConfigKey() + ".generateDumpOnLoad", false, &ok);
    if (m_generateDumpOnLoad) {
        m_crashdumper = static_cast<WindowsCrashDumpGenerator*>(s2e()->getPlugin("WindowsCrashDumpGenerator"));
        if (!m_manager) {
            s2e()->getWarningsStream() << "NDISHANDLERS: generateDumpOnLoad option requires the WindowsCrashDumpGenerator plugin!"
                    << '\n';
            exit(-1);
        }
    }



    m_windowsMonitor->onModuleUnload.connect(
            sigc::mem_fun(*static_cast<WindowsApi*>(this),
                    &WindowsApi::onModuleUnload)
            );
}





////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////

void NdisHandlers::NdisAllocateMemoryWithTag(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();
    NdisAllocateMemoryBase(state, fns);
}

void NdisHandlers::NdisAllocateMemoryWithTagRet(S2EExecutionState* state, uint32_t Address, uint32_t Length)
{
    //Call the normal allocator annotation, since both functions are similar
    NdisAllocateMemoryRet(state, Address, Length);
}

void NdisHandlers::NdisAllocateMemory(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    NdisAllocateMemoryBase(state, fns);
}

void NdisHandlers::NdisAllocateMemoryBase(S2EExecutionState* state, FunctionMonitorState *fns)
{

    state->undoCallAndJumpToSymbolic();

    bool ok = true;
    uint32_t Address, Length;
    ok &= readConcreteParameter(state, 0, &Address);
    ok &= readConcreteParameter(state, 1, &Length);
    if(!ok) {
        s2e()->getDebugStream(state) << "Can not read address and length of memory allocation" << '\n';
        return;
    }

    if (getConsistency(__FUNCTION__) < LOCAL) {
        //We'll have to grant access to the memory array
        FUNCMON_REGISTER_RETURN_A(state, m_functionMonitor, NdisHandlers::NdisAllocateMemoryRet, Address, Length);
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__)+"_failure");

    //Skip the call in the current state
    state->bypassFunction(3);

    //The doc also specifies that the address must be null in case of a failure
    uint32_t null = 0;
    bool suc = state->writeMemoryConcrete(Address, &null, sizeof(null));
    assert(suc);

    uint32_t failValue = 0xC0000001;
    state->writeCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &failValue, sizeof(failValue));

    //Register the return handler
    S2EExecutionState *otherState = states[0] == state ? states[1] : states[0];
    FUNCMON_REGISTER_RETURN_A(otherState, m_functionMonitor, NdisHandlers::NdisAllocateMemoryRet, Address, Length);
}

void NdisHandlers::NdisAllocateMemoryRet(S2EExecutionState* state, uint32_t Address, uint32_t Length)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    //Get the return value
    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
        return;
    }

    if (eax) {
        HANDLER_TRACE_FCNFAILED();
        //The original function has failed
        return;
    }

    if(m_memoryChecker) {
        uint32_t BufAddress;
        bool ok = state->readMemoryConcrete(Address, &BufAddress, 4);
        if(!ok) {
            s2e()->getWarningsStream() << __FUNCTION__ << ": cannot read allocated address" << '\n';
            return;
        }
        m_memoryChecker->grantMemory(state, BufAddress, Length, MemoryChecker::READWRITE,
                                     "ndis:alloc:NdisAllocateMemory", BufAddress);
    }
}

void NdisHandlers::NdisAllocateMemoryWithTagPriority(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    bool ok = true;
    uint32_t Length;
    ok &= readConcreteParameter(state, 1, &Length);
    if(!ok) {
        s2e()->getDebugStream(state) << __FUNCTION__ << ": can not read address params" << '\n';
        return;
    }
    FUNCMON_REGISTER_RETURN_A(state, fns, NdisHandlers::NdisAllocateMemoryWithTagPriorityRet, Length);
}

void NdisHandlers::NdisAllocateMemoryWithTagPriorityRet(S2EExecutionState* state, uint32_t Length)
{
    HANDLER_TRACE_RETURN();

    if(m_memoryChecker) {
        //Get the return value
        uint32_t eax;
        if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
            s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
            return;
        }

        if (!eax) {
            //The original function has failed
            return;
        }

        m_memoryChecker->grantMemory(state, eax, Length,
                                     MemoryChecker::READWRITE,
                                     "ndis:alloc:NdisAllocateMemoryWithTagPriority", eax);
    }
}

void NdisHandlers::NdisFreeMemory(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if(!m_memoryChecker) {
        return;
    }

    bool ok = true;
    uint32_t Address, Length;
    ok &= readConcreteParameter(state, 0, &Address);
    ok &= readConcreteParameter(state, 1, &Length);
    if(!ok) {
        s2e()->getWarningsStream() << __FUNCTION__ << ": can not read params" << '\n';
    }

    uint64_t AllocatedAddress, AllocatedLength;

    if (!m_memoryChecker->findMemoryRegion(state, Address, &AllocatedAddress, &AllocatedLength)) {
        s2e()->getExecutor()->terminateStateEarly(*state, "NdisFreeMemory: Tried to free an unallocated memory region");
    }

    if (AllocatedLength != Length) {
        std::stringstream ss;
        ss << "NdisFreeMemory called with length=0x" << std::hex << Length
           << " but was allocated 0x" << AllocatedLength;
        s2e()->getExecutor()->terminateStateEarly(*state, ss.str());
    }

    m_memoryChecker->revokeMemory(state, "*", Address);
}

void NdisHandlers::NdisMFreeSharedMemory(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    bool ok = true;

    uint32_t physAddr;
    uint32_t virtAddr;
    uint32_t length;

    //XXX: Physical address should be 64 bits
    ok &= readConcreteParameter(state, 1, &length); //Length
    ok &= readConcreteParameter(state, 3, &virtAddr); //VirtualAddress
    ok &= readConcreteParameter(state, 4, &physAddr); //PhysicalAddress

    if (!ok) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": could not read parameters" << '\n';
        return;
    }

    m_hw->resetSymbolicMmioRange(state, physAddr, length);

    if(m_memoryChecker) {
        m_memoryChecker->revokeMemory(state, virtAddr, length);
    }
}


void NdisHandlers::NdisMAllocateSharedMemory(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();


    bool ok = true;
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    ok &= readConcreteParameter(state, 1, &plgState->val3); //Length
    ok &= readConcreteParameter(state, 3, &plgState->val1); //VirtualAddress
    ok &= readConcreteParameter(state, 4, &plgState->val2); //PhysicalAddress

    if (!ok) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": could not read parameters" << '\n';
        return;
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMAllocateSharedMemoryRet)
}

void NdisHandlers::NdisMAllocateSharedMemoryRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    bool ok=true;
    uint32_t va = 0;
    uint64_t pa = 0;

    ok &= state->readMemoryConcrete(plgState->val1, &va, sizeof(va));
    ok &= state->readMemoryConcrete(plgState->val2, &pa, sizeof(pa));
    if (!ok) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": could not read returned addresses" << '\n';
        s2e()->getWarningsStream() << "VirtualAddress=" << hexval(va) << " PhysicalAddress=" << hexval(pa) << '\n';
        return;
    }

    if (!va) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": original call has failed" << '\n';
        return;
    }

    //Register symbolic DMA memory.
    //All reads from it will be symbolic.
    m_hw->setSymbolicMmioRange(state, pa, plgState->val3);

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }


    if (getConsistency(__FUNCTION__) == LOCAL || getConsistency(__FUNCTION__) == OVERAPPROX) {
        bool oldForkStatus = state->isForkingEnabled();
        state->enableForking();

        std::stringstream ss;
        ss << __FUNCTION__ << "_success";
        klee::ref<klee::Expr> succ = state->createSymbolicValue(klee::Expr::Int8, ss.str());
        klee::ref<klee::Expr> cond = klee::EqExpr::create(succ, klee::ConstantExpr::create(1, klee::Expr::Int8));
        klee::ref<klee::Expr> outcome =
                klee::SelectExpr::create(cond, klee::ConstantExpr::create(va, klee::Expr::Int32),
                                             klee::ConstantExpr::create(0, klee::Expr::Int32));
        state->writeMemory(plgState->val1, outcome);

        /* Fork success and failure */
        klee::Executor::StatePair sp = s2e()->getExecutor()->fork(*state, cond, false);

        S2EExecutionState *ts = static_cast<S2EExecutionState *>(sp.first);
        S2EExecutionState *fs = static_cast<S2EExecutionState *>(sp.second);
        m_functionMonitor->eraseSp(state == fs ? ts : fs, state->getPc());

        ts->setForking(oldForkStatus);
        fs->setForking(oldForkStatus);

        if(m_memoryChecker) {
            m_memoryChecker->grantMemory(ts, va, plgState->val3,
                                         MemoryChecker::READWRITE,
                                         "ndis:hw:NdisMAllocateSharedMemory");
        }
     }
}

void NdisHandlers::NdisAllocatePacket(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    uint32_t pStatus, pPacket;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    ok &= readConcreteParameter(state, 1, &pPacket);

    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
        return;
    }

    FUNCMON_REGISTER_RETURN_A(state, fns, NdisHandlers::NdisAllocatePacketRet, pStatus, pPacket);
}

void NdisHandlers::NdisAllocatePacketRet(S2EExecutionState* state, uint32_t pStatus, uint32_t pPacket)
{
    HANDLER_TRACE_RETURN();

    bool ok = true;
    NDIS_STATUS Status;
    uint32_t Packet, Length;

    ok &= state->readMemoryConcrete(pStatus, &Status, sizeof(Status));
    ok &= state->readMemoryConcrete(pPacket, &Packet, sizeof(Packet));
    ok &= state->readMemoryConcrete(Packet+4, &Length, 4);
    if(!ok) {
        s2e()->getDebugStream() << "Can not read result" << '\n';
        return;
    }

    if(Status) {
        return;
    }

    //if(Length<0x1000)
    //    Length = 0x1000; // XXX

    if(m_memoryChecker) {
        m_memoryChecker->grantMemory(state, Packet, Length, MemoryChecker::READWRITE,
                                     "ndis:alloc:NdisAllocatePacket");
    }
}

void NdisHandlers::NdisFreePacket(S2EExecutionState *state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    uint32_t packet;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &packet);

    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
        return;
    }

    if(m_memoryChecker) {
        m_memoryChecker->revokeMemory(state, packet, uint64_t(-1));
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisAllocateBufferPool(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t pStatus;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    if (!ok) {
        HANDLER_TRACE_FCNFAILED();
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Skip the call in the current state
    state->bypassFunction(3);

    //Write symbolic status code
    state->writeMemory(pStatus, createFailure(state, getVariableName(state, __FUNCTION__) + "_result"));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisAllocatePacketPool(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t pStatus;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    if (!ok) {
        HANDLER_TRACE_FCNFAILED();
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;
        vec.push_back(NDIS_STATUS_RESOURCES);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
    }
    state->writeMemory(pStatus, symb);

    //Skip the call in the current state
    state->bypassFunction(4);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisAllocatePacketPoolEx(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t pStatus;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    if (!ok) {
        HANDLER_TRACE_FCNFAILED();
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Write symbolic status code
    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;
        vec.push_back(NDIS_STATUS_RESOURCES);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), symb);
    }
    state->writeMemory(pStatus, symb);

    //Skip the call in the current state
    state->bypassFunction(5);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
//XXX: Avoid copy/pasting of code with NdisOpenAdapter and other similar functions.
void NdisHandlers::NdisOpenConfiguration(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t pStatus;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    if (!ok) {
        HANDLER_TRACE_FCNFAILED();
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Write symbolic status code
    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;
        //This is a success code. Might cause problems later...
        vec.push_back(NDIS_STATUS_FAILURE);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
    }
    state->writeMemory(pStatus, symb);

    //Skip the call in the current state
    state->bypassFunction(3);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisMQueryAdapterInstanceName(S2EExecutionState* state, FunctionMonitorState *fns)
{
    NdisQueryAdapterInstanceName(state, fns);
}

void NdisHandlers::NdisQueryAdapterInstanceName(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        //XXX: Register a return handler and grant access rights
        return;
    }

    state->undoCallAndJumpToSymbolic();

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Write symbolic status code
    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;
        vec.push_back(NDIS_STATUS_RESOURCES);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
    }
    //state->writeMemory(pStatus, symb);

    state->writeCpuRegister(CPU_OFFSET(regs[R_EAX]), symb);

    //Skip the call in the current state
    state->bypassFunction(2);

    //Register a return handler in the normal state
    S2EExecutionState *successState = states[0] == state ? states[1] : states[0];

    uint32_t pUnicodeString;
    if (readConcreteParameter(state, 0, &pUnicodeString)) {
        FUNCMON_REGISTER_RETURN_A(successState, fns, NdisHandlers::NdisQueryAdapterInstanceNameRet, pUnicodeString);
    }else {
        HANDLER_TRACE_PARAM_FAILED(0);
    }
}

void NdisHandlers::NdisQueryAdapterInstanceNameRet(S2EExecutionState* state, uint64_t pUnicodeString)
{
    HANDLER_TRACE_RETURN();
    if (!pUnicodeString) {
        return;
    }

    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
        return;
    }

    if(eax) {
        s2e()->getDebugStream() << __FUNCTION__ << " failed with 0x" << hexval(eax) << '\n';
        return;
    }

    NDIS_STRING s;
    bool ok = true;
    ok = state->readMemoryConcrete(pUnicodeString, &s, sizeof(s));
    if(!ok) {
        s2e()->getDebugStream() << "Can not read NDIS_STRING" << '\n';
        return;
    }

    if(m_memoryChecker) {
        m_memoryChecker->grantMemory(state, s.Buffer, s.Length, MemoryChecker::READ,
                             "ndis:ret:NdisMQueryAdapterInstanceName");
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisQueryPendingIOCount(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Write symbolic status code
    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;

        //XXX: check this one...
        vec.push_back(NDIS_STATUS_CLOSING);
        vec.push_back(NDIS_STATUS_FAILURE);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
    }
    state->writeCpuRegister(CPU_OFFSET(regs[R_EAX]), symb);

    //Skip the call in the current state
    state->bypassFunction(2);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisOpenAdapter(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) < LOCAL) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t pStatus;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    if (!ok) {
        HANDLER_TRACE_FCNFAILED();
        return;
    }

    //Fork one successful state and one failed state (where the function is bypassed)
    std::vector<S2EExecutionState *> states;
    forkStates(state, states, 1, getVariableName(state, __FUNCTION__) + "_failure");

    //Write symbolic status code
    klee::ref<klee::Expr> symb;
    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        symb = createFailure(state, getVariableName(state, __FUNCTION__) + "_result");
    }else {
        std::vector<uint32_t> vec;
        //This is a success code. Might cause problems later...
        //vec.push_back(NDIS_STATUS_PENDING);
        vec.push_back(NDIS_STATUS_RESOURCES);
        vec.push_back(NDIS_STATUS_ADAPTER_NOT_FOUND);
        vec.push_back(NDIS_STATUS_UNSUPPORTED_MEDIA);
        vec.push_back(NDIS_STATUS_CLOSING);
        vec.push_back(NDIS_STATUS_OPEN_FAILED);
        symb = addDisjunctionToConstraints(state, getVariableName(state, __FUNCTION__) + "_result", vec);
    }
    state->writeMemory(pStatus, symb);

    //Skip the call in the current state
    state->bypassFunction(11);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisAllocateBuffer(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    uint32_t pStatus, pBuffer, Length;
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &pStatus);
    ok &= readConcreteParameter(state, 1, &pBuffer);
    ok &= readConcreteParameter(state, 2, &Length);

    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
        return;
    }

    FUNCMON_REGISTER_RETURN_A(state, fns, NdisHandlers::NdisAllocateBufferRet, pStatus, pBuffer, Length);
}

void NdisHandlers::NdisAllocateBufferRet(S2EExecutionState* state, uint32_t pStatus, uint32_t pBuffer, uint32_t Length)
{
    HANDLER_TRACE_RETURN();

    bool ok = true;
    NDIS_STATUS Status;
    uint32_t Buffer;

    ok &= state->readMemoryConcrete(pStatus, &Status, sizeof(Status));
    ok &= state->readMemoryConcrete(pBuffer, &Buffer, sizeof(Buffer));
    if(!ok) {
        s2e()->getDebugStream() << "Can not read result" << '\n';
        return;
    }

    if(Status) {
        return;
    }

    //Length += 0x1000; // XXX

    if(m_memoryChecker) {
        m_memoryChecker->grantMemory(state, Buffer, Length, MemoryChecker::READWRITE,
                                     "ndis:alloc:NdisAllocateBuffer");
    }
}



////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisMInitializeTimer(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    if (!readConcreteParameter(state, 2, &plgState->val1)) {
        s2e()->getDebugStream() << "Could not read function pointer for timer entry point" << '\n';
        return;
    }

    uint32_t priv;
    if (!readConcreteParameter(state, 3, &priv)) {
        s2e()->getDebugStream() << "Could not read private pointer for timer entry point" << '\n';
        return;
    }

    s2e()->getDebugStream(state) << "NdisMInitializeTimer pc=0x" << hexval(plgState->val1) <<
            " priv=0x" << priv << '\n';

    m_timerEntryPoints.insert(std::make_pair(plgState->val1, priv));

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMInitializeTimerRet)
}

void NdisHandlers::NdisMInitializeTimerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    NDIS_REGISTER_ENTRY_POINT(plgState->val1, NdisTimerEntryPoint);
}

//This annotation will try to run all timer entry points at once to maximize coverage.
//This is only for overapproximate consistency.
void NdisHandlers::NdisTimerEntryPoint(S2EExecutionState* state, FunctionMonitorState *fns)
{
    static TimerEntryPoints exploredRealEntryPoints;
    static TimerEntryPoints exploredEntryPoints;
    TimerEntryPoints scheduled;

    state->undoCallAndJumpToSymbolic();

    HANDLER_TRACE_CALL();

    //state->dumpStack(20, state->getSp());

    uint32_t realPc = state->getPc();
    uint32_t realPriv = 0;
    if (!readConcreteParameter(state, 1, &realPriv)) {
        s2e()->getDebugStream(state) << "Could not read value of opaque pointer" << '\n';
        return;
    }
    s2e()->getDebugStream(state) << "realPc=" << hexval(realPc) << " realpriv=" << hexval(realPriv) << '\n';

    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisTimerEntryPointRet)
        return;
    }


    //If this this entry point is called for real for the first time,
    //schedule it for execution.
    if (exploredRealEntryPoints.find(std::make_pair(realPc, realPriv)) == exploredRealEntryPoints.end()) {
        s2e()->getDebugStream(state) << "Never called for real, schedule for execution" << '\n';
        exploredRealEntryPoints.insert(std::make_pair(realPc, realPriv));
        exploredEntryPoints.insert(std::make_pair(realPc, realPriv));
        scheduled.insert(std::make_pair(realPc, realPriv));
    }

    //Compute the set of timer entry points that were never executed.
    //These ones will be scheduled for fake execution.
    TimerEntryPoints scheduleFake;
    std::insert_iterator<TimerEntryPoints > ii(scheduleFake, scheduleFake.begin());
    std::set_difference(m_timerEntryPoints.begin(), m_timerEntryPoints.end(),
                        exploredEntryPoints.begin(), exploredEntryPoints.end(),
                        ii);

    scheduled.insert(scheduleFake.begin(), scheduleFake.end());
    exploredEntryPoints.insert(scheduled.begin(), scheduled.end());

    //If all timers explored, return
    if (scheduled.size() == 0) {
        s2e()->getDebugStream(state) << "No need to redundantly run the timer another time" << '\n';
        state->bypassFunction(4);
        throw CpuExitException();
    }

    //If a timer is scheduled, make sure that the real one is also there.
    //Otherwise, there may be a crash, and all states could wrongly terminate.
    if (scheduled.size() > 0) {
       if (scheduled.find(std::make_pair(realPc, realPriv)) == scheduled.end()) {
           scheduled.insert(std::make_pair(realPc, realPriv));
       }
    }

    //Must register return handler before forking.
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisTimerEntryPointRet)

    //Fork the number of states corresponding to the number of timers
    std::vector<S2EExecutionState*> states;
    forkStates(state, states, scheduled.size() - 1, __FUNCTION__);
    assert(states.size() == scheduled.size());

    //Fetch the physical address of the first parameter.
    //XXX: This is a hack. S2E does not support writing to virtual memory
    //of inactive states.
    uint32_t param = 1; //We want the second parameter
    uint64_t physAddr = state->getPhysicalAddress(state->getSp() + (param+1) * sizeof(uint32_t));


    //Force the exploration of all registered timer entry points here.
    //This speeds up the exploration
    unsigned stateIdx = 0;
    foreach2(it, scheduled.begin(), scheduled.end()) {
        S2EExecutionState *curState = states[stateIdx++];

        uint32_t pc = (*it).first;
        uint32_t priv = (*it).second;

        s2e()->getDebugStream() << "Found timer " << hexval(pc) << " with private struct " << hexval(priv)
         << " to explore." << '\n';

        //Overwrite the original private field with the new one
        klee::ref<klee::Expr> privExpr = klee::ConstantExpr::create(priv, klee::Expr::Int32);
        curState->writeMemory(physAddr, privExpr, S2EExecutionState::PhysicalAddress);

        //Overwrite the program counter with the new handler
        curState->writeCpuState(offsetof(CPUState, eip), pc, sizeof(uint32_t)*8);

        //Mark wheter this state will explore a fake call or a real one.
        DECLARE_PLUGINSTATE(NdisHandlersState, curState);
        plgState->faketimer = !(pc == realPc && priv == realPriv);
    }


}

void NdisHandlers::NdisTimerEntryPointRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    //We must terminate states that ran fake calls, otherwise the system might crash later.
    if (plgState->faketimer) {
        s2e()->getExecutor()->terminateStateEarly(*state, "Terminating state with fake timer call");
    }

    if (plgState->cableStatus == NdisHandlersState::DISCONNECTED) {
        s2e()->getExecutor()->terminateStateEarly(*state, "Terminating state because cable is disconnected");
    }

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisSetTimer(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();


    uint32_t interval;
    if (!readConcreteParameter(state, 1, &interval)) {
        s2e()->getDebugStream() << "Could not read timer interval" << '\n';
        return;
    }

    //We make the interval longer to avoid overloading symbolic execution.
    //This should give more time for the rest of the system to execute.
    s2e()->getDebugStream() << "Setting interval to " << interval*m_timerIntervalFactor
            <<" ms. Was " << interval << " ms." << '\n';

    klee::ref<klee::Expr> newInterval = klee::ConstantExpr::create(interval*m_timerIntervalFactor, klee::Expr::Int32);
    writeParameter(state, 1, newInterval);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisMRegisterAdapterShutdownHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();


    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    if (!readConcreteParameter(state, 2, &plgState->val1)) {
        s2e()->getDebugStream() << "Could not read function pointer for timer entry point" << '\n';
        return;
    }

    plgState->shutdownHandler = plgState->val1;

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMRegisterAdapterShutdownHandlerRet)
}

void NdisHandlers::NdisMRegisterAdapterShutdownHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    NDIS_REGISTER_ENTRY_POINT(plgState->val1, NdisShutdownEntryPoint);
}

void NdisHandlers::NdisShutdownEntryPoint(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisShutdownEntryPointRet)
}

void NdisHandlers::NdisShutdownEntryPointRet(S2EExecutionState* state)
{
    HANDLER_TRACE_CALL();
    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisMMapIoSpace(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMMapIoSpaceRet)
}

void NdisHandlers::NdisMMapIoSpaceRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    if (getConsistency(__FUNCTION__) == LOCAL) {
        std::vector<uint32_t> values;

        values.push_back(NDIS_STATUS_SUCCESS);
        values.push_back(NDIS_STATUS_RESOURCE_CONFLICT);
        values.push_back(NDIS_STATUS_RESOURCES);
        values.push_back(NDIS_STATUS_FAILURE);
        forkRange(state, __FUNCTION__, values);
    }else  {
        klee::ref<klee::Expr> success = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), success);
    }
}

void NdisHandlers::NdisMAllocateMapRegisters(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMAllocateMapRegistersRet)
}

void NdisHandlers::NdisMAllocateMapRegistersRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUX86State, regs[R_EAX]), &eax, sizeof(eax))) {
        return;
    }
    if (eax != NDIS_STATUS_SUCCESS) {
        s2e()->getDebugStream(state) <<  __FUNCTION__ << " failed" << '\n';
        return;
    }

    if (getConsistency(__FUNCTION__) == LOCAL) {
        std::vector<uint32_t> values;

        values.push_back(NDIS_STATUS_SUCCESS);
        values.push_back(NDIS_STATUS_RESOURCES);
        forkRange(state, __FUNCTION__, values);
    }else {
        klee::ref<klee::Expr> success = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), success);
    }
}

void NdisHandlers::NdisMSetAttributesEx(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    klee::ref<klee::Expr> interfaceType = readParameter(state, 4);
    s2e()->getDebugStream(state) << "InterfaceType: " << interfaceType << '\n';

    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        return;
    }

    if (((signed)m_forceAdapterType) != InterfaceTypeUndefined) {
        s2e()->getDebugStream(state) << "Forcing NIC type to " << m_forceAdapterType << '\n';
        writeParameter(state, 4, klee::ConstantExpr::create(m_forceAdapterType, klee::Expr::Int32));
    }
}


void NdisHandlers::NdisMSetAttributes(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    klee::ref<klee::Expr> interfaceType = readParameter(state, 3);
    s2e()->getDebugStream(state) << "InterfaceType: " << interfaceType << '\n';

    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        return;
    }

    if (((signed)m_forceAdapterType) != InterfaceTypeUndefined) {
        s2e()->getDebugStream(state) << "Forcing NIC type to " << m_forceAdapterType << '\n';
        writeParameter(state, 3, klee::ConstantExpr::create(m_forceAdapterType, klee::Expr::Int32));
    }

}

void NdisHandlers::NdisReadConfiguration(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    //Save parameter data that we will use on return
    //We need to put them in the state-local storage, as parameters can be mangled by the caller
    bool ok = true;
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    ok &= readConcreteParameter(state, 0, &plgState->pStatus);
    ok &= readConcreteParameter(state, 1, &plgState->pConfigParam);
    ok &= readConcreteParameter(state, 3, &plgState->pConfigString);

    if(m_memoryChecker) {
        if (!ok) {
            s2e()->getDebugStream() << __FUNCTION__ << " could not read stack parameters (maybe symbolic?) "  << '\n';
            return;
        }
    }

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    if (!ok) {
        s2e()->getDebugStream() << __FUNCTION__ << " could not read stack parameters (maybe symbolic?) "  << '\n';
        return;
    }

    uint64_t pc = 0;
    state->getReturnAddress(&pc);
    const ModuleDescriptor *md = m_detector->getModule(state, pc, true);
    if (md) {
        pc = md->ToNativeBase(pc);
    }

    std::string keyword;
    ok = ReadUnicodeString(state, plgState->pConfigString, keyword);
    if (ok) {
        uint32_t paramType;
        ok &= readConcreteParameter(state, 4, &paramType);

        s2e()->getMessagesStream() << "NdisReadConfiguration Module=" << (md ? md->Name : "<unknown>") <<
                " pc=" << hexval(pc) <<
                " Keyword=" << keyword <<
            " Type=" << paramType  << '\n';
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisReadConfigurationRet)
}

void NdisHandlers::NdisReadConfigurationRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    if (!plgState->pStatus) {
        s2e()->getDebugStream() << "Status is NULL!" << '\n';
        return;
    }

    klee::ref<klee::Expr> Status = state->readMemory(plgState->pStatus, klee::Expr::Int32);
    if (!NtSuccess(s2e(), state, Status)) {
        s2e()->getDebugStream() << __FUNCTION__ << " failed with " << Status << '\n';
        return;
    }


    bool ok = true;
    uint32_t pConfigParam;

    ok &= state->readMemoryConcrete(plgState->pConfigParam, &pConfigParam, sizeof(pConfigParam));
    if (!ok || !pConfigParam) {
        s2e()->getDebugStream() << "Could not read pointer to configuration data" << Status << '\n';
        return;
    }

    std::string configString;
    ok = ReadUnicodeString(state, plgState->pConfigString, configString);
    if (!ok) {
        s2e()->getDebugStream() << "Could not read keyword string" << '\n';
    }

    //In all consistency models, inject symbolic value in the parameter that was read
    NDIS_CONFIGURATION_PARAMETER ConfigParam;
    ok = state->readMemoryConcrete(pConfigParam, &ConfigParam, sizeof(ConfigParam));
    if (ok) {

        if (m_ignoreKeywords.find(configString) == m_ignoreKeywords.end()) {

            //For now, we only inject integer values
            if (ConfigParam.ParameterType == NdisParameterInteger || ConfigParam.ParameterType == NdisParameterHexInteger) {
                //Write the symbolic value there.
                uint32_t valueOffset = offsetof(NDIS_CONFIGURATION_PARAMETER, ParameterData);
                std::stringstream ss;
                ss << __FUNCTION__ << "_" << configString << "_value";
                klee::ref<klee::Expr> val = state->createSymbolicValue(klee::Expr::Int32, ss.str());
                state->writeMemory(pConfigParam + valueOffset, val);
            }
        }
    }else {
        s2e()->getDebugStream() << "Could not read configuration data" << Status << '\n';
        //Continue, this error is not too bad.
    }

    if (getConsistency(__FUNCTION__) == LOCAL) {
        //Fork with either success or failure
        //XXX: Since we cannot write to memory of inactive states, simply create a bunch of select statements
        std::stringstream ss;
        ss << __FUNCTION__ << "_" << configString <<"_success";
        klee::ref<klee::Expr> succ = state->createSymbolicValue(klee::Expr::Bool, ss.str());
        klee::ref<klee::Expr> cond = klee::EqExpr::create(succ, klee::ConstantExpr::create(1, klee::Expr::Bool));
        klee::ref<klee::Expr> outcome =
                klee::SelectExpr::create(cond, klee::ConstantExpr::create(NDIS_STATUS_SUCCESS, klee::Expr::Int32),
                                             klee::ConstantExpr::create(NDIS_STATUS_FAILURE, klee::Expr::Int32));
        state->writeMemory(plgState->pStatus, outcome);

        bool oldForkStatus = state->isForkingEnabled();
        state->enableForking();

        klee::Executor::StatePair sp = s2e()->getExecutor()->fork(*state, cond, false);

        S2EExecutionState *ts = static_cast<S2EExecutionState *>(sp.first);
        S2EExecutionState *fs = static_cast<S2EExecutionState *>(sp.second);
        m_functionMonitor->eraseSp(state == fs ? ts : fs, state->getPc());

        /* Update each of the states */
        if(m_memoryChecker) {
            m_memoryChecker->grantMemory(ts, pConfigParam, sizeof(ConfigParam),
                                         MemoryChecker::READ,
                                         "ndis:ret:NdisReadConfiguration");
            if(ConfigParam.ParameterType == NdisParameterString ||
                    ConfigParam.ParameterType == NdisParameterMultiString) {
                m_memoryChecker->grantMemory(ts,
                                             ConfigParam.ParameterData.StringData.Buffer,
                                             ConfigParam.ParameterData.StringData.Length,
                                             MemoryChecker::READ,
                                             "ndis:ret:NdisReadConfiguration:StringData");
            } else if(ConfigParam.ParameterType == NdisParameterBinary) {
                m_memoryChecker->grantMemory(ts,
                                             ConfigParam.ParameterData.BinaryData.Buffer,
                                             ConfigParam.ParameterData.BinaryData.Length,
                                             MemoryChecker::READ,
                                             "ndis:ret:NdisReadConfiguration:BinaryData");
            }
        }

        ts->setForking(oldForkStatus);
        fs->setForking(oldForkStatus);

    }else if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        std::stringstream ss;
        ss << __FUNCTION__ << "_success";
        klee::ref<klee::Expr> val = state->createSymbolicValue(klee::Expr::Int32, ss.str());
        state->writeMemory(plgState->pStatus, val);
    }

    plgState->pNetworkAddress = 0;
    plgState->pStatus = 0;
    plgState->pNetworkAddressLength = 0;



}

void NdisHandlers::NdisCloseConfiguration(S2EExecutionState *state, FunctionMonitorState *fns)
{
    if(m_memoryChecker) {
        m_memoryChecker->revokeMemory(state, "ndis:ret:NdisReadConfiguration*");
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
//BEWARE: This is a VarArg cdecl function...
void NdisHandlers::NdisWriteErrorLogEntry(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();
    std::stringstream os;

    uint32_t ErrorCode, NumberOfErrorValues;
    bool ok = true;

    ok &= readConcreteParameter(state, 1, &ErrorCode);
    ok &= readConcreteParameter(state, 2, &NumberOfErrorValues);

    if (!ok) {
        os << "Could not read error parameters" << '\n';
        return;
    }

    os << "ErrorCode=0x" << std::hex << ErrorCode << " - ";

    for (unsigned i=0; i<NumberOfErrorValues; ++i) {
        uint32_t val;
        ok &= readConcreteParameter(state, 3+i, &val);
        if (!ok) {
            os << "Could not read error parameters" << '\n';
            break;
        }
        os << val << " ";
    }

    os << '\n';

    s2e()->getDebugStream() << os.str();

}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisReadPciSlotInformation(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (!m_devDesc) {
        s2e()->getWarningsStream() << __FUNCTION__ << " needs a valid symbolic device" << '\n';
        return;
    }

    uint32_t slot, offset, buffer, length;
    bool ok = true;

    ok &= readConcreteParameter(state, 1, &slot);
    ok &= readConcreteParameter(state, 2, &offset);
    ok &= readConcreteParameter(state, 3, &buffer);
    ok &= readConcreteParameter(state, 4, &length);
    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
    }

    s2e()->getDebugStream(state) << "Buffer=" << hexval(buffer) <<
        " Length=" << length << " Slot=" << slot << " Offset=" << hexval(offset) << '\n';


    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint64_t retaddr;
    ok = state->getReturnAddress(&retaddr);


    uint8_t buf[256];
    bool readConcrete = false;

    //Check if we access the base address registers
    if (offset >= 0x10 && offset < 0x28 && (offset - 0x10 + length <= 0x28)) {
        //Get the real concrete values
        readConcrete = m_devDesc->readPciAddressSpace(buf, offset, length);
    }

    //Fill in the buffer with symbolic values
    for (unsigned i=0; i<length; ++i) {
        if (readConcrete) {
            state->writeMemory(buffer + i, &buf[i], klee::Expr::Int8);
        }else {
            std::stringstream ss;
            ss << __FUNCTION__ << "_0x" << std::hex << retaddr << "_"  << i;
            klee::ref<klee::Expr> symb = state->createSymbolicValue(klee::Expr::Int8, ss.str());
            state->writeMemory(buffer+i, symb);
        }
    }

    //Symbolic return value
    std::stringstream ss;
    ss << __FUNCTION__ << "_" <<  std::hex << (retaddr) << "_retval";
    klee::ref<klee::Expr> symb = state->createSymbolicValue(klee::Expr::Int32, ss.str());
    klee::ref<klee::Expr> expr = klee::UleExpr::create(symb, klee::ConstantExpr::create(length, klee::Expr::Int32));
    state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), symb);
    state->addConstraint(expr);

    state->bypassFunction(5);
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisWritePciSlotInformation(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();
    
    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    state->undoCallAndJumpToSymbolic();

    uint32_t length;
    bool ok = true;

    ok &= readConcreteParameter(state, 4, &length);
    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
    }

    uint64_t retaddr;
    ok = state->getReturnAddress(&retaddr);

    s2e()->getDebugStream(state) << 
        " Length=" << length << '\n';

    //Symbolic return value
    std::stringstream ss;
    ss << __FUNCTION__ << "_" << std::hex << retaddr << "_retval";
    klee::ref<klee::Expr> symb = state->createSymbolicValue(klee::Expr::Int32, ss.str());
    klee::ref<klee::Expr> expr = klee::UleExpr::create(symb, klee::ConstantExpr::create(length, klee::Expr::Int32));
    state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), symb);
    state->addConstraint(expr);

    state->bypassFunction(5);
    throw CpuExitException();
}

void NdisHandlers::NdisMQueryAdapterResources(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    
    bool ok = true;
    ok &= readConcreteParameter(state, 0, &plgState->pStatus);

    //XXX: Do the other parameters as well
    if (!ok) {
        s2e()->getDebugStream(state) << "Could not read parameters" << '\n';
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMQueryAdapterResourcesRet)
}

void NdisHandlers::NdisMQueryAdapterResourcesRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    
    if (!plgState->pStatus) {
        s2e()->getDebugStream() << "Status is NULL!" << '\n';
        return;
    }

    klee::ref<klee::Expr> Status = state->readMemory(plgState->pStatus, klee::Expr::Int32);
    if (!NtSuccess(s2e(), state, Status)) {
        s2e()->getDebugStream() << __FUNCTION__ << " failed with " << Status << '\n';
        return;
    }

    klee::ref<klee::Expr> SymbStatus = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
    state->writeMemory(plgState->pStatus, SymbStatus);

    return;
}


void NdisHandlers::NdisMRegisterInterrupt(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        //Pretend the interrupt is shared, to force the ISR to be called.
        //Make sure there is indeed a miniportisr registered
        DECLARE_PLUGINSTATE(NdisHandlersState, state);
        if (plgState->hasIsrHandler) {
            s2e()->getDebugStream() << "Pretending that the interrupt is shared." << '\n';
            //Overwrite the parameter value here
            klee::ref<klee::ConstantExpr> val = klee::ConstantExpr::create(1, klee::Expr::Int32);
            writeParameter(state, 5, val);
        }
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMRegisterInterruptRet)
}

void NdisHandlers::NdisMRegisterInterruptRet(S2EExecutionState* state)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    //Get the return value
    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
        return;
    }

    if (eax) {
        //The original function has failed
        s2e()->getDebugStream() << __FUNCTION__ << ": original function failed with " << hexval(eax) << '\n';
        return;
    }

    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        klee::ref<klee::Expr> success = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), success);
    }else

    //Consistency: LOCAL
    if (getConsistency(__FUNCTION__) == LOCAL) {
        std::vector<uint32_t> values;

        values.push_back(NDIS_STATUS_SUCCESS);
        values.push_back(NDIS_STATUS_RESOURCE_CONFLICT);
        values.push_back(NDIS_STATUS_RESOURCES);
        values.push_back(NDIS_STATUS_FAILURE);
        forkRange(state, __FUNCTION__, values);
    }
}

void NdisHandlers::NdisMRegisterIoPortRange(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMRegisterIoPortRangeRet)
}

void NdisHandlers::NdisMRegisterIoPortRangeRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    //Get the return value
    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
        return;
    }

    if (eax) {
        //The original function has failed
        return;
    }

    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        klee::ref<klee::Expr> success = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), success);
    }else

    //Consistency: LOCAL
    if (getConsistency(__FUNCTION__) == LOCAL) {
        std::vector<uint32_t> values;

        values.push_back(NDIS_STATUS_SUCCESS);
        values.push_back(NDIS_STATUS_RESOURCE_CONFLICT);
        values.push_back(NDIS_STATUS_RESOURCES);
        values.push_back(NDIS_STATUS_FAILURE);
        forkRange(state, __FUNCTION__, values);
    }

}

void NdisHandlers::NdisReadNetworkAddress(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    //Save parameter data that we will use on return
    //We need to put them in the state-local storage, as parameters can be mangled by the caller
    bool ok = true;
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    ok &= readConcreteParameter(state, 0, &plgState->pStatus);
    ok &= readConcreteParameter(state, 1, &plgState->pNetworkAddress);
    ok &= readConcreteParameter(state, 2, &plgState->pNetworkAddressLength);

    if (!ok) {
        s2e()->getDebugStream() << __FUNCTION__ << " could not read stack parameters (maybe symbolic?) "  << '\n';
        return;
    }

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisReadNetworkAddressRet)
}

void NdisHandlers::NdisReadNetworkAddressRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    if (!plgState->pStatus) {
        s2e()->getDebugStream() << "Status is NULL!" << '\n';
        return;
    }

    klee::ref<klee::Expr> Status = state->readMemory(plgState->pStatus, klee::Expr::Int32);
    if (!NtSuccess(s2e(), state, Status)) {
        s2e()->getDebugStream() << __FUNCTION__ << " failed with " << Status << '\n';
        return;
    }


    bool ok = true;
    uint32_t Length, NetworkAddress;

    ok &= state->readMemoryConcrete(plgState->pNetworkAddressLength, &Length, sizeof(Length));
    ok &= state->readMemoryConcrete(plgState->pNetworkAddress, &NetworkAddress, sizeof(NetworkAddress));
    if (!ok || !NetworkAddress) {
        s2e()->getDebugStream() << "Could not read network address pointer and/or its length" << Status << '\n';
        return;
    }

    //In all cases, inject symbolic values in the returned buffer
    for (unsigned i=0; i<Length; ++i) {

        if (m_networkAddress.size() > 0) {
            s2e()->getDebugStream() << "NetworkAddress[" << i << "]=" <<
              hexval(m_networkAddress[i % m_networkAddress.size()]) << '\n';

            state->writeMemoryConcrete(NetworkAddress + i, &m_networkAddress[i % m_networkAddress.size()], 1);
        }else {
            std::stringstream ss;
            ss << __FUNCTION__ << "_" << i;
            klee::ref<klee::Expr> val = state->createSymbolicValue(klee::Expr::Int8, ss.str());
            state->writeMemory(NetworkAddress + i, val);
        }
    }

    if (getConsistency(__FUNCTION__) == LOCAL) {
        //Fork with either success or failure
        //XXX: Since we cannot write to memory of inactive states, simply create a bunch of select statements
        std::stringstream ss;
        ss << __FUNCTION__ << "_success";
        klee::ref<klee::Expr> succ = state->createSymbolicValue(klee::Expr::Bool, ss.str());
        klee::ref<klee::Expr> cond = klee::EqExpr::create(succ, klee::ConstantExpr::create(1, klee::Expr::Bool));
        klee::ref<klee::Expr> outcome =
                klee::SelectExpr::create(cond, klee::ConstantExpr::create(NDIS_STATUS_SUCCESS, klee::Expr::Int32),
                                             klee::ConstantExpr::create(NDIS_STATUS_FAILURE, klee::Expr::Int32));
        state->writeMemory(plgState->pStatus, outcome);

    }else if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        std::stringstream ss;
        ss << __FUNCTION__ << "_success";
        klee::ref<klee::Expr> val = state->createSymbolicValue(klee::Expr::Int32, ss.str());
        state->writeMemory(plgState->pStatus, val);
    }

    plgState->pNetworkAddress = 0;
    plgState->pStatus = 0;
    plgState->pNetworkAddressLength = 0;

}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////

void NdisHandlers::NdisMRegisterMiniport(S2EExecutionState* state, FunctionMonitorState *fns)
{
    if (!calledFromModule(state)) { return; }
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) != STRICT) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMRegisterMiniportRet)
    }

    //Extract the function pointers from the passed data structure
    uint32_t pMiniport;
    if (!state->readMemoryConcrete(state->getSp() + sizeof(pMiniport) * (1+1), &pMiniport, sizeof(pMiniport))) {
        s2e()->getMessagesStream() << "Could not read pMiniport address from the stack" << '\n';
        return;
    }

    s2e()->getMessagesStream() << "NDIS_MINIPORT_CHARACTERISTICS @0x" << pMiniport << '\n';

    s2e::windows::NDIS_MINIPORT_CHARACTERISTICS32 Miniport;
    if (!state->readMemoryConcrete(pMiniport, &Miniport, sizeof(Miniport))) {
        s2e()->getMessagesStream() << "Could not read NDIS_MINIPORT_CHARACTERISTICS" << '\n';
        return;
    }

    //Register each handler
    NDIS_REGISTER_ENTRY_POINT(Miniport.CheckForHangHandler, CheckForHang);
    NDIS_REGISTER_ENTRY_POINT(Miniport.InitializeHandler, InitializeHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.DisableInterruptHandler, DisableInterruptHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.EnableInterruptHandler, EnableInterruptHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.HaltHandler, HaltHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.HandleInterruptHandler, HandleInterruptHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.ISRHandler, ISRHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.QueryInformationHandler, QueryInformationHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.ReconfigureHandler, ReconfigureHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.ResetHandler, ResetHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.SendHandler, SendHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.SendPacketsHandler, SendPacketsHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.SetInformationHandler, SetInformationHandler);
    NDIS_REGISTER_ENTRY_POINT(Miniport.TransferDataHandler, TransferDataHandler);

    if (Miniport.ISRHandler) {
        DECLARE_PLUGINSTATE(NdisHandlersState, state);
        plgState->hasIsrHandler = true;
    }
}

void NdisHandlers::NdisMRegisterMiniportRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    //Get the return value
    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << '\n';
        return;
    }

    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        //Replace the return value with a symbolic value
        if ((int)eax>=0) {
            klee::ref<klee::Expr> ret = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
            state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), ret);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
//These are driver entry points
////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::NdisMStatusHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::NdisMStatusHandlerRet)
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    uint32_t status;
    if (!readConcreteParameter(state, 1, &status)) {
        s2e()->getDebugStream() << "Could not get cable status in " << __FUNCTION__ << '\n';
        return;
    }

    s2e()->getDebugStream() << "Status is " << hexval(status) << '\n';

    if (status == NDIS_STATUS_MEDIA_CONNECT) {
        s2e()->getDebugStream() << "Cable is connected" << '\n';
        plgState->cableStatus = NdisHandlersState::CONNECTED;
    }else if (status == NDIS_STATUS_MEDIA_DISCONNECT) {
        s2e()->getDebugStream() << "Cable is disconnected" << '\n';
        plgState->cableStatus = NdisHandlersState::DISCONNECTED;
    }
}   
    
void NdisHandlers::NdisMStatusHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::CheckForHang(S2EExecutionState* state, FunctionMonitorState *fns)
{
    static bool exercised = false;

    HANDLER_TRACE_CALL();

    if (exercised) {
        uint32_t success = 1;
        state->writeCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &success, sizeof(success));
        state->bypassFunction(1);
        throw CpuExitException();
    }

    exercised = true;
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::CheckForHangRet)
}

void NdisHandlers::CheckForHangRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    if (getConsistency(__FUNCTION__) == OVERAPPROX) {
        //Pretend we did not hang
        //uint32_t success = 0;
        //state->writeCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &success, sizeof(success));
    }

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::InitializeHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::InitializeHandlerRet)

    /* Make the medium array symbolic */
    uint32_t pMediumArray, MediumArraySize;

    if (!readConcreteParameter(state, 2, &pMediumArray)) {
        s2e()->getDebugStream(state) << "Could not read pMediumArray" << '\n';
        return;
    }

    if (!readConcreteParameter(state, 3, &MediumArraySize)) {
        s2e()->getDebugStream(state) << "Could not read MediumArraySize" << '\n';
        return;
    }

    //Register API exported in the handle
    uint32_t NdisHandle;
    if (!readConcreteParameter(state, 4, &NdisHandle)) { 
        s2e()->getDebugStream(state) << "Could not read NdisHandle" << '\n';
        return;
    }
    
    uint32_t pStatusHandler;
    if (!state->readMemoryConcrete(NdisHandle + 0x17c, &pStatusHandler, sizeof(pStatusHandler))) {
        s2e()->getMessagesStream() << "Could not read pointer to status handler" << '\n';
        return;
    }

    s2e()->getDebugStream() << "NDIS_M_STATUS_HANDLER " << hexval(pStatusHandler) << '\n';

    if(m_memoryChecker) {
        //const ModuleDescriptor* module = m_detector->getModule(state, state->getPc());
        m_memoryChecker->grantMemory(state, pMediumArray, MediumArraySize*4,
                             MemoryChecker::READ, "ndis:args:MiniportInitialize:MediumArray");
    }

    NDIS_REGISTER_ENTRY_POINT(pStatusHandler, NdisMStatusHandler);



    if (getConsistency(__FUNCTION__) == STRICT) {
        return;
    }

    //if (getConsistency(__FUNCTION__) == LOCAL)
    {
        //Make size properly constrained
        if (pMediumArray) {
            for (unsigned i=0; i<MediumArraySize; i++) {
                std::stringstream ss;
                ss << "MediumArray" << std::dec << "_" << i;
                state->writeMemory(pMediumArray + i * 4, state->createSymbolicValue(klee::Expr::Int32, ss.str()));
            }

            klee::ref<klee::Expr> SymbSize = state->createSymbolicValue(klee::Expr::Int32, "MediumArraySize");

            klee::ref<klee::Expr> Constr = klee::UleExpr::create(SymbSize,
                                                               klee::ConstantExpr::create(MediumArraySize, klee::Expr::Int32));
            state->addConstraint(Constr);
            writeParameter(state, 3, SymbSize);
        }
    }

}

void NdisHandlers::InitializeHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    //const ModuleDescriptor* module = m_detector->getModule(state, state->getPc());

    if(m_memoryChecker) {
        m_memoryChecker->revokeMemory(state, "ndis:args:MiniportInitialize:*");
    }

    //Check the success status, kill if failure
    klee::ref<klee::Expr> eax = state->readCpuRegister(offsetof(CPUState, regs[R_EAX]), klee::Expr::Int32);
    klee::Solver *solver = s2e()->getExecutor()->getSolver();
    bool isTrue;
    klee::ref<klee::Expr> eq = klee::EqExpr::create(eax, klee::ConstantExpr::create(0, eax.get()->getWidth()));
    if (!solver->mayBeTrue(klee::Query(state->constraints, eq), isTrue)) {
        s2e()->getMessagesStream(state) << "Killing state "  << state->getID() <<
                " because InitializeHandler failed to determine success" << '\n';
        s2e()->getExecutor()->terminateStateEarly(*state, "InitializeHandler solver failed");
    }

    if (!isTrue) {
        s2e()->getMessagesStream(state) << "Killing state "  << state->getID() <<
                " because InitializeHandler failed with " << eax << '\n';
        if(m_memoryChecker) {
            // Driver should free all resources if initialization fails
            m_memoryChecker->checkMemoryLeaks(state);
        }
        s2e()->getExecutor()->terminateStateEarly(*state, "InitializeHandler failed");
        return;
    }

    //Make sure we succeed by adding a constraint on the eax value
    klee::ref<klee::Expr> constr = klee::SgeExpr::create(eax, klee::ConstantExpr::create(0, eax.get()->getWidth()));
    state->addConstraint(constr);

    s2e()->getDebugStream(state) << "InitializeHandler succeeded with " << eax << '\n';

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::DisableInterruptHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    if (!m_devDesc) {
        s2e()->getWarningsStream() << __FUNCTION__ << " needs a valid symbolic device" << '\n';
        return;
    }


    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::DisableInterruptHandlerRet)
    m_devDesc->setInterrupt(false);
}

void NdisHandlers::DisableInterruptHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::EnableInterruptHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::EnableInterruptHandlerRet)
}

void NdisHandlers::EnableInterruptHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::HaltHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();

    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::HaltHandlerRet)
        return;
    }

    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    if (!plgState->shutdownHandler) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::HaltHandlerRet);
        return;
    }

    state->undoCallAndJumpToSymbolic();

    bool oldForkStatus = state->isForkingEnabled();
    state->enableForking();

    //Fork the states. In one of them run the shutdown handler
    klee::ref<klee::Expr> isFake = state->createSymbolicValue(klee::Expr::Int8, "FakeShutdown");
    klee::ref<klee::Expr> cond = klee::EqExpr::create(isFake, klee::ConstantExpr::create(1, klee::Expr::Int8));

    klee::Executor::StatePair sp = s2e()->getExecutor()->fork(*state, cond, false);

    S2EExecutionState *ts = static_cast<S2EExecutionState *>(sp.first);
    S2EExecutionState *fs = static_cast<S2EExecutionState *>(sp.second);

    //One of the states will run the shutdown handler
    ts->writeCpuState(offsetof(CPUState, eip), plgState->shutdownHandler, sizeof(uint32_t)*8);

    FUNCMON_REGISTER_RETURN(ts, fns, NdisHandlers::HaltHandlerRet)
    FUNCMON_REGISTER_RETURN(fs, fns, NdisHandlers::HaltHandlerRet)

    ts->setForking(oldForkStatus);
    fs->setForking(oldForkStatus);
}

void NdisHandlers::HaltHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    if(m_memoryChecker) {
        m_memoryChecker->checkMemoryLeaks(state);
    }

    //There is nothing more to execute, kill the state
    s2e()->getExecutor()->terminateStateEarly(*state, "NdisHalt");

}
////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::HandleInterruptHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::HandleInterruptHandlerRet)
    DECLARE_PLUGINSTATE(NdisHandlersState, state);
    plgState->isrHandlerExecuted = true;
    plgState->isrHandlerQueued = false;
}

void NdisHandlers::HandleInterruptHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ISRHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    if (!m_devDesc) {
        s2e()->getWarningsStream() << __FUNCTION__ << " needs a valid symbolic device" << '\n';
        return;
    }


    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::ISRHandlerRet)

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    bool ok = true;
    ok &= readConcreteParameter(state, 0, &plgState->isrRecognized);
    ok &= readConcreteParameter(state, 1, &plgState->isrQueue);

    if (!ok) {
        s2e()->getDebugStream(state) << "Error reading isrRecognized and isrQueue"<< '\n';
    }
    m_devDesc->setInterrupt(false);
}

void NdisHandlers::ISRHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    if (!m_devDesc) {
        s2e()->getWarningsStream() << __FUNCTION__ << " needs a valid symbolic device" << '\n';
        return;
    }


    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    uint8_t isrRecognized=0, isrQueue=0;
    bool ok = true;
    ok &= state->readMemoryConcrete(plgState->isrRecognized, &isrRecognized, sizeof(isrRecognized));
    ok &= state->readMemoryConcrete(plgState->isrQueue, &isrQueue, sizeof(isrQueue));

    s2e()->getDebugStream(state) << "ISRRecognized=" << (bool)isrRecognized <<
            " isrQueue="<< (bool)isrQueue << '\n';

    if (!ok) {
        s2e()->getExecutor()->terminateStateEarly(*state, "Could not determine whether the NDIS driver queued the interrupt");
    }else {
        //Kill the state if no interrupt will ever occur in it.
        if ((!isrRecognized || !isrQueue) && (!plgState->isrHandlerExecuted && !plgState->isrHandlerQueued)) {
            s2e()->getExecutor()->terminateStateEarly(*state, "The state will not trigger any interrupt");
        }

        if (isrRecognized && isrQueue) {
            plgState->isrHandlerQueued = true;
        }
    }

    m_devDesc->setInterrupt(false);
    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::QuerySetInformationHandler(S2EExecutionState* state, FunctionMonitorState *fns, bool isQuery)
{
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    s2e()->getDebugStream() << "Called with OID=" << hexval(plgState->oid) << '\n';

    if (getConsistency(__FUNCTION__) != OVERAPPROX) {
        return;
    }

    std::stringstream ss;
    ss << __FUNCTION__ << "_OID";
    klee::ref<klee::Expr> symbOid = state->createSymbolicValue(klee::Expr::Int32, ss.str());

    klee::ref<klee::Expr> isFakeOid = state->createSymbolicValue(klee::Expr::Int8, "IsFakeOid");
    klee::ref<klee::Expr> cond = klee::EqExpr::create(isFakeOid, klee::ConstantExpr::create(1, klee::Expr::Int8));
/*    klee::ref<klee::Expr> outcome =
            klee::SelectExpr::create(cond, symbOid,
                                         klee::ConstantExpr::create(plgState->oid, klee::Expr::Int32));
*/

    //We fake the stack pointer and prepare symbolic inputs for the buffer
    uint32_t original_sp = state->getSp();
    uint32_t current_sp = original_sp;

    //Create space for the new buffer
    //This will also be present in the original call.
    uint32_t newBufferSize = 64;
    klee::ref<klee::Expr> symbBufferSize = state->createSymbolicValue(klee::Expr::Int32, "QuerySetInfoBufferSize");
    current_sp -= newBufferSize;
    uint32_t newBufferPtr = current_sp;

    //XXX: Do OID-aware injection
    for (unsigned i=0; i<newBufferSize; ++i) {
        std::stringstream ssb;
        ssb << __FUNCTION__ << "_buffer_" << i;
        klee::ref<klee::Expr> symbByte = state->createSymbolicValue(klee::Expr::Int8, ssb.str());
        state->writeMemory(current_sp + i, symbByte);
    }

    //Copy and patch the parameters
    uint32_t origContext, origInfoBuf, origLength, origBytesWritten, origBytesNeeded;
    bool b = true;
    b &= readConcreteParameter(state, 0, &origContext);
    b &= readConcreteParameter(state, 2, &origInfoBuf);
    b &= readConcreteParameter(state, 3, &origLength);
    b &= readConcreteParameter(state, 4, &origBytesWritten);
    b &= readConcreteParameter(state, 5, &origBytesNeeded);

    if (b) {
        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, (uint8_t*)&origBytesNeeded, klee::Expr::Int32);
        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, (uint8_t*)&origBytesWritten, klee::Expr::Int32);

        //Symbolic buffer size
        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, symbBufferSize);


        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, (uint8_t*)&newBufferPtr, klee::Expr::Int32);
        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, symbOid);
        current_sp -= sizeof(uint32_t);
        b &= state->writeMemory(current_sp, (uint8_t*)&origContext, klee::Expr::Int32);

        //Push the new return address (it does not matter normally, so put NULL)
        current_sp -= sizeof(uint32_t);
        uint32_t retaddr = 0x0badcafe;
        b &= state->writeMemory(current_sp,(uint8_t*)&retaddr, klee::Expr::Int32);

    }

    bool oldForkStatus = state->isForkingEnabled();
    state->enableForking();

    //Fork now, because we do not need to access memory anymore
    klee::Executor::StatePair sp = s2e()->getExecutor()->fork(*state, cond, false);

    S2EExecutionState *ts = static_cast<S2EExecutionState *>(sp.first);
    S2EExecutionState *fs = static_cast<S2EExecutionState *>(sp.second);

    //Save which state is fake
    DECLARE_PLUGINSTATE_N(NdisHandlersState, ht, ts);
    DECLARE_PLUGINSTATE_N(NdisHandlersState, hf, fs);

    ht->fakeoid = true;
    hf->fakeoid = false;

    //Set the new stack pointer for the fake state
    ts->writeCpuRegisterConcrete(offsetof(CPUState, regs[R_ESP]), &current_sp, sizeof(current_sp));

    //Add a constraint for the buffer size
    klee::ref<klee::Expr> symbBufferConstr = klee::UleExpr::create(symbBufferSize, klee::ConstantExpr::create(newBufferSize, klee::Expr::Int32));
    ts->addConstraint(symbBufferConstr);

    //Register separately the return handler,
    //since the stack pointer is different in the two states
    FUNCMON_REGISTER_RETURN(ts, fns, NdisHandlers::QueryInformationHandlerRet)
    FUNCMON_REGISTER_RETURN(fs, fns, NdisHandlers::QueryInformationHandlerRet)

    ts->setForking(oldForkStatus);
    fs->setForking(oldForkStatus);
}

void NdisHandlers::QuerySetInformationHandlerRet(S2EExecutionState* state, bool isQuery)
{
    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    if (plgState->fakeoid) {
        //Stop inconsistent execution immediately
        s2e()->getExecutor()->terminateStateEarly(*state, "Killing state with fake OID");
    }

    s2e()->getDebugStream(state) << "State is not fake, continuing..." << '\n';

    if (isQuery) {
        //Keep only those states that have a connected cable
        uint32_t status;
        if (state->readMemoryConcrete(plgState->pInformationBuffer, &status, sizeof(status))) {
            s2e()->getDebugStream() << "Status=" << hexval(status) << '\n';
            if (plgState->oid == OID_GEN_MEDIA_CONNECT_STATUS) {
                s2e()->getDebugStream(state) << "OID_GEN_MEDIA_CONNECT_STATUS is " << status << '\n';
                if (status == 1) {
                   //Disconnected, kill the state
                   //XXX: For now, we force it to be connected, this is a problem for consistency !!!
                    //It must be connected, otherwise NDIS will not forward any packet to the driver!
                    status = 0;
                    state->writeMemoryConcrete(plgState->pInformationBuffer, &status, sizeof(status));
                  //s2e()->getExecutor()->terminateStateEarly(*state, "Media is disconnected");
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::QueryInformationHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    //XXX: what about multiple driver?
    static bool alreadyExplored = false;

    HANDLER_TRACE_CALL();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    plgState->oid = (uint32_t)-1;
    plgState->pInformationBuffer = 0;

    readConcreteParameter(state, 1, &plgState->oid);
    readConcreteParameter(state, 2, &plgState->pInformationBuffer);

    s2e()->getDebugStream(state) << "OID=" << hexval(plgState->oid) << '\n';

    if (alreadyExplored) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::QueryInformationHandlerRet)
        s2e()->getDebugStream(state) << "Already explored " << __FUNCTION__ << " at " << hexval(state->getPc()) << '\n';
        return;
    }

    state->undoCallAndJumpToSymbolic();

    alreadyExplored = true;
    QuerySetInformationHandler(state, fns, true);

}

void NdisHandlers::QueryInformationHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
    state->jumpToSymbolicCpp();

    QuerySetInformationHandlerRet(state, true);

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::SetInformationHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    static bool alreadyExplored = false;

    HANDLER_TRACE_CALL();

    DECLARE_PLUGINSTATE(NdisHandlersState, state);

    plgState->oid = (uint32_t)-1;
    plgState->pInformationBuffer = 0;

    readConcreteParameter(state, 1, &plgState->oid);
    readConcreteParameter(state, 2, &plgState->pInformationBuffer);

    s2e()->getDebugStream(state) << "OID=" << hexval(plgState->oid) << '\n';

    if (alreadyExplored) {
        FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::SetInformationHandlerRet)
        s2e()->getDebugStream(state) << "Already explored " << __FUNCTION__ << " at " << hexval(state->getPc()) << '\n';
        return;
    }

    state->undoCallAndJumpToSymbolic();

    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::SetInformationHandlerRet)

    alreadyExplored = true;
    QuerySetInformationHandler(state, fns, false);
}

void NdisHandlers::SetInformationHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    QuerySetInformationHandlerRet(state, false);

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ReconfigureHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::ReconfigureHandlerRet)
}

void NdisHandlers::ReconfigureHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ResetHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::ResetHandlerRet)
}

void NdisHandlers::ResetHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::SendHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::SendHandlerRet)
}

void NdisHandlers::SendHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    if (m_devDesc) {
        m_devDesc->setInterrupt(true);
    }

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::SendPacketsHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::SendPacketsHandlerRet)
}

void NdisHandlers::SendPacketsHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();

    m_devDesc->setInterrupt(true);

    m_manager->succeedState(state);
    m_functionMonitor->eraseSp(state, state->getPc());
    throw CpuExitException();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::TransferDataHandler(S2EExecutionState* state, FunctionMonitorState *fns)
{
    HANDLER_TRACE_CALL();
    FUNCMON_REGISTER_RETURN(state, fns, NdisHandlers::TransferDataHandlerRet)
}

void NdisHandlers::TransferDataHandlerRet(S2EExecutionState* state)
{
    HANDLER_TRACE_RETURN();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////


NdisHandlersState::NdisHandlersState()
{
    pStatus = 0;
    pNetworkAddress = 0;
    pNetworkAddressLength = 0;
    hasIsrHandler = false;
    fakeoid = false;
    isrRecognized = 0;
    isrQueue = 0;
    isrHandlerExecuted = false;
    isrHandlerQueued = false;
    faketimer = false;
    shutdownHandler = 0;   
    cableStatus = UNKNOWN;
}

NdisHandlersState::~NdisHandlersState()
{

}

NdisHandlersState* NdisHandlersState::clone() const
{
    return new NdisHandlersState(*this);
}

PluginState *NdisHandlersState::factory(Plugin *p, S2EExecutionState *s)
{
    return new NdisHandlersState();
}


} // namespace plugins
} // namespace s2e
