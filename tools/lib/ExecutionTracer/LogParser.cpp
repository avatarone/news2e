#include <iostream>
#include <cassert>
#include "LogParser.h"

#ifdef _WIN32

#else

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>



#endif

//#define DEBUG_LP

using namespace s2e::plugins;

namespace s2etools
{

void LogEvents::processItem(unsigned currentItem,
                         const s2e::plugins::ExecutionTraceItemHeader &hdr,
                         void *data)
{
    assert(hdr.type < TRACE_MAX);

#ifdef DEBUG_LP
    std::cerr << "Item " << currentItem << " sid=" << (int)hdr.stateId <<
            " type=" << (int) hdr.type << std::endl;
#endif

    onEachItem.emit(currentItem, hdr, (void*)data);
}

LogEvents::LogEvents()
{

}

LogEvents::~LogEvents()
{

}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

LogParser::LogParser():LogEvents()
{
    m_File = NULL;
    m_size = 0;

    m_cachedProcessor = NULL;
    m_cachedState = NULL;
}

LogParser::~LogParser()
{
    if (m_File) {
        munmap(m_File, m_size);
    }
}



bool LogParser::parse(const std::string &fileName)
{
#ifdef _WIN32
#error Implement memory-mapped file support
#else
    int file = open(fileName.c_str(), O_RDONLY);
    if (file<0) {
        std::cerr << "LogParser: Could not open " << fileName << std::endl;
        return false;
    }

    off_t fileSize = lseek(file, 0, SEEK_END);
    if (fileSize == (off_t) -1) {
        std::cerr << "Could not get log file size" << std::endl;
        return false;
    }

    m_File = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, file, 0);
    if (!m_File) {
        std::cerr << "Could not map the log file in memory" << std::endl;
        close(file);
        return false;
    }

    m_size = fileSize;

#endif


    m_ItemOffsets.clear();

    uint64_t currentOffset = 0;
    unsigned currentItem = 0;

    uint8_t *buffer = (uint8_t*)m_File;

    while(currentOffset < m_size) {

        s2e::plugins::ExecutionTraceItemHeader *hdr =
                (s2e::plugins::ExecutionTraceItemHeader *)(buffer);

        if (currentOffset + sizeof(s2e::plugins::ExecutionTraceItemHeader) > m_size) {
            std::cerr << "LogParser: Could not read header " << std::endl;
            return false;
        }

        buffer += sizeof(*hdr);

        if (hdr->size > 0) {
            if (currentOffset + hdr->size > m_size) {
                std::cerr << "LogParser: Could not read payload " << std::endl;
                return false;
            }
        }

        processItem(currentItem, *hdr, buffer);
        buffer+=hdr->size;

        m_ItemOffsets.push_back(currentOffset);

        currentOffset += sizeof(s2e::plugins::ExecutionTraceItemHeader)  + hdr->size;

        ++currentItem;
    }

    //fclose(file);
    return true;
}

bool LogParser::getItem(unsigned index, s2e::plugins::ExecutionTraceItemHeader &hdr, void **data)
{
    if (!m_File) {
        assert(false);
        return false;
    }

    if (index >= m_ItemOffsets.size() ) {
        assert(false);
        return false;
    }

    uint64_t offset = m_ItemOffsets[index];
    uint8_t *buffer = (uint8_t*)m_File + offset;
    hdr = *(s2e::plugins::ExecutionTraceItemHeader*)buffer;

    *data = NULL;
    if (hdr.size > 0) {
        *data = buffer + sizeof(s2e::plugins::ExecutionTraceItemHeader);
    }

    return true;
}

ItemProcessorState* LogParser::getState(void *processor, ItemProcessorStateFactory f)
{
    if (processor == m_cachedProcessor) {
        return m_cachedState;
    }

    ItemProcessorState *ret;
    ItemProcessors::const_iterator it = m_ItemProcessors.find(processor);
    if (it == m_ItemProcessors.end()) {
        ret = f();
        m_ItemProcessors[processor] = ret;
    } else {
        ret = (*it).second;
    }

    m_cachedProcessor = processor;
    m_cachedState = ret;
    return ret;
}

ItemProcessorState* LogParser::getState(void *processor, uint32_t pathId)
{
    assert(pathId == 0);
    ItemProcessors::const_iterator it = m_ItemProcessors.find(processor);
    if (it == m_ItemProcessors.end()) {
        return NULL;
    } else {
        return (*it).second;
    }
}

//A flat trace has only one path
void LogParser::getPaths(PathSet &s)
{
    s.clear();
    s.insert(0);
}


}
