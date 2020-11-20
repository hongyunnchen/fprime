// ======================================================================
// \title  BufferManagerComponentImpl.cpp
// \author tcanham
// \brief  cpp file for BufferManager component implementation class
//
// \copyright
// Copyright 2009-2015, by the California Institute of Technology.
// ALL RIGHTS RESERVED.  United States Government Sponsorship
// acknowledged.
//
// ======================================================================


#include <Svc/BufferManager/BufferManagerComponentImpl.hpp>
#include "Fw/Types/BasicTypes.hpp"
#include <Fw/Types/Assert.hpp>
#include <new>
#include <stdio.h>

namespace Svc {

  // ----------------------------------------------------------------------
  // Construction, initialization, and destruction
  // ----------------------------------------------------------------------

  BufferManagerComponentImpl ::
    BufferManagerComponentImpl(
        const char *const compName
    ) : BufferManagerComponentBase(compName)
    ,m_setup(false)
    ,m_mgrID(0)
    ,m_buffers(0)
    ,m_allocator(0)
    ,m_identifier(0)
    ,m_numStructs(0)
    ,m_highWater(0)
    ,m_currBuffs(0)
    ,m_noBuffs(0)
    ,m_emptyBuffs(0)
  {

  }

  void BufferManagerComponentImpl ::
    init(
        const NATIVE_INT_TYPE instance
    )
  {
    BufferManagerComponentBase::init(instance);
  }

  BufferManagerComponentImpl ::
    ~BufferManagerComponentImpl(void)
  {

  }

  // ----------------------------------------------------------------------
  // Handler implementations for user-defined typed input ports
  // ----------------------------------------------------------------------

  void BufferManagerComponentImpl ::
    bufferSendIn_handler(
        const NATIVE_INT_TYPE portNum,
        Fw::Buffer &fwBuffer
    )
  {
      // make sure component has been set up
      FW_ASSERT(this->m_setup);
      // check for empty buffers - this is just a warning since this component returns
      // empty buffers if it can't allocate one.
      if (fwBuffer.getsize() == 0) {
          this->log_WARNING_HI_ZeroSizeBuffer();
          this->tlmWrite_EmptyBuffs(++this->m_emptyBuffs);
          return;
      }
      // use the bufferID member field to find the original slot
      U32 id = fwBuffer.getbufferID();
      // check some things
      FW_ASSERT(id < this->m_numStructs,id,this->m_numStructs);
      FW_ASSERT(fwBuffer.getmanagerID() == this->m_mgrID);
      FW_ASSERT(true == this->m_buffers[id].allocated);
      FW_ASSERT(reinterpret_cast<U8*>(fwBuffer.getdata()) == this->m_buffers[id].memory);
      // user can make smaller for their own purposes, but it shouldn't be bigger
      FW_ASSERT(fwBuffer.getsize() <= this->m_buffers[id].size);
      // clear the allocated flag
      this->m_buffers[id].allocated = false;
      // reset the size
      this->m_buffers[id].buff.setsize(this->m_buffers[id].size);
      this->tlmWrite_CurrBuffs(--this->m_currBuffs);

  }

  Fw::Buffer BufferManagerComponentImpl ::
    bufferGetCallee_handler(
        const NATIVE_INT_TYPE portNum,
        U32 size
    )
  {
      // make sure component has been set up
      FW_ASSERT(this->m_setup);
      this->tlmWrite_TotalBuffs(this->m_numStructs);
      // find smallest buffer based on size.
      for (NATIVE_UINT_TYPE buff = 0; buff < this->m_numStructs; buff++) {
          if ((not this->m_buffers[buff].allocated) and (size < this->m_buffers[buff].size)) {
              this->m_buffers[buff].allocated = true;
              this->tlmWrite_CurrBuffs(++this->m_currBuffs);
              if (this->m_currBuffs > this->m_highWater) {
                  this->m_highWater = this->m_currBuffs;
                  this->tlmWrite_HiBuffs(this->m_highWater);
              }
              return this->m_buffers[buff].buff;
          }
      }

      // if no buffers found, return empty buffer
      this->log_WARNING_HI_NoBuffsAvailable(size);
      this->tlmWrite_NoBuffs(++this->m_noBuffs);
      return Fw::Buffer();

  }

  void BufferManagerComponentImpl::setup(
    NATIVE_UINT_TYPE mgrID, //!< manager ID
    NATIVE_UINT_TYPE memID, //!< Memory segment identifier
    Fw::MemAllocator& allocator, //!< memory allocator
    const BufferBins& bins //!< Set of user bins
  ) {

    this->m_mgrID = mgrID;
    this->m_identifier = memID;
    this->m_allocator = &allocator;
    // clear bins
    memset(&this->m_bufferBins,0,sizeof(this->m_bufferBins));

    this->m_bufferBins = bins;

    // compute the amount of memory needed
    NATIVE_UINT_TYPE memorySize = 0; // size needed memory
    this->m_numStructs = 0; // size the number of tracking structs
    // walk through bins and add up the sizes
    for (NATIVE_UINT_TYPE bin = 0; bin < MAX_NUM_BINS; bin++) {
        if (this->m_bufferBins.bins[bin].numBuffers) {
            memorySize += 
                this->m_bufferBins.bins[bin].bufferSize * this->m_bufferBins.bins[bin].numBuffers // allocate each set of buffer memory
                + sizeof(AllocatedBuffer) * this->m_bufferBins.bins[bin].numBuffers; // allocate the structs to track the buffers
            this->m_numStructs += this->m_bufferBins.bins[bin].numBuffers;
        }
    }

    NATIVE_UINT_TYPE allocatedSize = memorySize;
    bool recoverable; //!< don't care if it is recoverable since they are a pool of user buffers

    // allocate memory
    void *memory = allocator.allocate(memID,allocatedSize,recoverable);
    // make sure the memory returns was non-zero and the size requested
    FW_ASSERT(memory);
    FW_ASSERT(memorySize == allocatedSize,memorySize,allocatedSize);
    // structs will be at beginning of memory
    this->m_buffers = static_cast<AllocatedBuffer*>(memory);
    // memory buffers will be at end of structs in memory, so compute that memory as the begining of the 
    // struct past the number of structs
    U8* bufferMem = reinterpret_cast<U8*>(&this->m_buffers[this->m_numStructs]);

    // walk through entries and initialize them
    NATIVE_UINT_TYPE currStruct = 0;
    for (NATIVE_UINT_TYPE bin = 0; bin < MAX_NUM_BINS; bin++) {
        if (this->m_bufferBins.bins[bin].numBuffers) {
            for (NATIVE_UINT_TYPE binEntry = 0; binEntry < this->m_bufferBins.bins[bin].numBuffers; binEntry++) {
                // placement new for Fw::Buffer instance. We don't need the new() return value, 
                // because we know where the Fw::Buffer instance is
                (void) new(&this->m_buffers[currStruct].buff) Fw::Buffer(mgrID,
                      currStruct,reinterpret_cast<U64>(bufferMem),this->m_bufferBins.bins[bin].bufferSize);
                this->m_buffers[currStruct].allocated = false;
                this->m_buffers[currStruct].memory = bufferMem;
                this->m_buffers[currStruct].size = this->m_bufferBins.bins[bin].bufferSize;
                bufferMem += this->m_bufferBins.bins[bin].bufferSize;
                currStruct++;
            }
        }
    }
      
    // check some assertions
    FW_ASSERT(bufferMem == (static_cast<U8*>(memory) + memorySize));
    FW_ASSERT(currStruct == this->m_numStructs,currStruct,this->m_numStructs);
    // indicate setup is done
    this->m_setup = true;
  }

} // end namespace Svc
