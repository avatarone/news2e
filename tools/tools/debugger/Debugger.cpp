#include "llvm/Support/CommandLine.h"
#include <stdio.h>
#include <inttypes.h>
#include <iostream>
#include <fstream>

#include <lib/ExecutionTracer/ModuleParser.h>
#include <lib/ExecutionTracer/Path.h>
#include <lib/ExecutionTracer/TestCase.h>
#include <lib/BinaryReaders/BFDInterface.h>

#include "Debugger.h"

extern "C" {
#include <bfd.h>
}

using namespace llvm;
using namespace s2etools;
using namespace s2e::plugins;

namespace {

    cl::opt<std::string>
        TraceFile("trace", cl::desc("Input trace"), cl::init("ExecutionTracer.dat"));

    cl::opt<std::string>
        LogFile("log", cl::desc("Store the analysis result in the log file"), cl::init("Debugger.dat"));

    cl::opt<unsigned>
        MemoryValue("memval", cl::desc("Memory value to look for"), cl::init(0));

    cl::opt<int>
        PathId("pathid", cl::desc("Which path to analyze (-1 for all)"), cl::init(0));

    cl::opt<std::string>
        ModPath("modpath", cl::desc("Path to module descriptors"), cl::init("."));

}

namespace s2etools {

MemoryDebugger::MemoryDebugger(BFDLibrary *lib, ModuleCache *cache, LogEvents *events, std::ostream &os) : m_os(os)
{
    m_events = events;
    m_connection = events->onEachItem.connect(
            sigc::mem_fun(*this, &MemoryDebugger::onItem)
            );
    m_cache = cache;
    m_library = lib;
}

MemoryDebugger::~MemoryDebugger()
{
    m_connection.disconnect();
}

void MemoryDebugger::printHeader(const s2e::plugins::ExecutionTraceItemHeader &hdr)
{
    m_os << "[State " << std::dec << hdr.stateId << "]";
}

void MemoryDebugger::doLookForValue(const s2e::plugins::ExecutionTraceItemHeader &hdr,
                                    const s2e::plugins::ExecutionTraceMemory &item)
{
  /*  if (!(item.flags & EXECTRACE_MEM_WRITE)) {
    if ((m_valueToFind && (item.value != m_valueToFind))) {
        return;
    }
}*/

    printHeader(hdr);
    m_os << " pc=0x" << std::hex << item.pc <<
            " addr=0x" << item.address <<
            " val=0x" << item.value <<
            " size=" << std::dec << (unsigned)item.size <<
            " iswrite=" << (item.flags & EXECTRACE_MEM_WRITE);

    const ModuleInstance *mi = m_cache->getInstance(hdr.pid, item.pc);
    std::string dbg;
    if (m_library->print(mi, item.pc, dbg, true, true, true)) {
        m_os << " - " << dbg;
    }

     m_os << std::endl;

}

void MemoryDebugger::doPageFault(const s2e::plugins::ExecutionTraceItemHeader &hdr,
                                 const s2e::plugins::ExecutionTracePageFault &item)
{
    printHeader(hdr);
    m_os << " pc=0x" << std::hex << item.pc <<
            " addr=0x" << item.address <<
            " iswrite=" << (bool)item.isWrite;

    const ModuleInstance *mi = m_cache->getInstance(hdr.pid, item.pc);
    std::string dbg;
    if (m_library->print(mi, item.pc, dbg, true, true, true)) {
        m_os << " - " << dbg;
    }

     m_os << std::endl;
}

void MemoryDebugger::onItem(unsigned traceIndex,
            const s2e::plugins::ExecutionTraceItemHeader &hdr,
            void *item)
{
    if (hdr.type == s2e::plugins::TRACE_MEMORY) {
       switch(m_analysisType)
        {
        case LOOK_FOR_VALUE:
            doLookForValue(hdr, *(const s2e::plugins::ExecutionTraceMemory*)item);
            break;
        default:
            break;
        }
    }else if (hdr.type == s2e::plugins::TRACE_PAGEFAULT) {
        doPageFault(hdr, *(const s2e::plugins::ExecutionTracePageFault*)item);
    }


}

/****************************************************************/
/****************************************************************/
/****************************************************************/

Debugger::Debugger(const std::string &file)
{
    m_fileName = file;
    m_library.setPath(ModPath);
    m_binaries.setPath(ModPath);
}

Debugger::~Debugger()
{

}

void Debugger::process()
{
    ExecutionPaths paths;
    std::ofstream logfile;
    logfile.open(LogFile.c_str());

    PathBuilder pb(&m_parser);
    m_parser.parse(m_fileName);

    pb.enumeratePaths(paths);

    PathBuilder::printPaths(paths, std::cout);

    ExecutionPaths::iterator pit;

    unsigned pathNum = 0;
    for(pit = paths.begin(); pit != paths.end(); ++pit) {
        if (PathId != -1) {
            if (pathNum != (unsigned)PathId) {
                continue;
            }
        }

        std::cout << "Analyzing path " << pathNum << std::endl;
        ModuleCache mc(&pb, &m_library);
        MemoryDebugger md(&m_binaries, &mc, &pb, logfile);

        md.lookForValue(MemoryValue);
        pb.processPath(*pit);
        ++pathNum;
    }
}

}

int main(int argc, char **argv)
{
    cl::ParseCommandLineOptions(argc, (char**) argv, " debugger");

    s2etools::Debugger dbg(TraceFile.getValue());
    dbg.process();

    return 0;
}

