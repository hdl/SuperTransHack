/**
 * @file
 * @author  jpoe   <>, (C) 2008, 2009
 * @date    09/19/08
 * @brief   This is the implementation for the global coherence module.
 *
 * @section LICENSE
 * Copyright: See COPYING file that comes with this distribution
 *
 * @section DESCRIPTION
 * C++ Implementation: transCoherence
 */
/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

#include "transCoherence.h"
#include "ThreadContext.h"
#include "transReport.h"

transCoherence *transGCM = 0;

/**
 * @ingroup transCoherence
 * @brief   Global Coherence Module
 */
transCoherence::transCoherence()
{
}

/**
 * @ingroup transCoherence
 * @brief   Constructor
 */
transCoherence::transCoherence(FILE* out, int conflicts, int versioning, int cacheLineSize)
{
  this->conflictDetection = conflicts;
  this->versioning = versioning;
  this->cacheLineSize = cacheLineSize;
  this->out = out;

   utid = 0; // Set Global Transaction ID = 0

  // Eager/Eager
  if(versioning && conflictDetection)
  {
    this->readPtr = &transCoherence::readEE;
    this->writePtr = &transCoherence::writeEE;
    this->beginPtr = &transCoherence::beginEE;
    this->commitPtr = &transCoherence::commitEE;
    this->abortPtr = &transCoherence::abortEE;
  }
  // Eager/Lazy rides on top of Eager/Eager with 
  // different stall times on commit/abort
  else if(!versioning && conflictDetection)
  {
    this->readPtr = &transCoherence::readEE;
    this->writePtr = &transCoherence::writeEE;
    this->beginPtr = &transCoherence::beginEE;
    this->commitPtr = &transCoherence::commitEE;
    this->abortPtr = &transCoherence::abortEE;
  }
  // Eager/Lazy
  else if(!versioning && !conflictDetection)
  {
    this->readPtr = &transCoherence::readLL;
    this->writePtr = &transCoherence::writeLL;
    this->beginPtr = &transCoherence::beginLL;
    this->commitPtr = &transCoherence::commitLL;
    this->abortPtr = &transCoherence::abortLL;
  }
  // Lazy/Lazy
  else
  {
    fprintf(stderr,"Unsupported Versioning/Conflict Detection combination provided!\n");
    exit(0);
  }

  for(int i = 0; i < MAX_CPU_COUNT; i++)
  {
    transState[i].timestamp = ((~0ULL) - 1024);
    transState[i].cycleFlag = 0;
    transState[i].state = INVALID;
    transState[i].beginPC = 0;
    stallCycle[i] = 0;
    abortCount[i] = 0;
    abortReason[i].first = 0;
    abortReason[i].second = 0;
    tmDepth[i] = 0;
    currentCommitter = -1;
  }

}

/**
 * @ingroup transCoherence
 * @brief   Create new cache state reference with Read bit set
 * 
 * @param pid   Process ID
 * @return     Cache state
 */
struct cacheState transCoherence::newReadState(int pid)
{
    struct cacheState tmp;
    tmp.state = R;
    tmp.readers.insert(pid);
    return tmp;
}

/**
 * @ingroup transCoherence
 * @brief   Create new cache state reference with Write bit set
 * 
 * @param pid   Process ID
 * @return Cache state
 */
struct cacheState transCoherence::newWriteState(int pid)
{
    struct cacheState tmp;
    tmp.state = W;
    tmp.writers.insert(pid);
    return tmp;
}

/**
 * @ingroup transCoherence
 * @brief check to see if thread has been ordered to abort
 * 
 * @param pid Process ID
 * @param tid Thread ID
 * @return Abort?
 */
bool transCoherence::checkAbort(int pid, int tid)
{
  if(transState[pid].state == DOABORT)
  {
    tmReport->reportAbort(transState[pid].utid,pid, tid, abortReason[pid].first, abortReason[pid].second, abortReason[pid].second,transState[pid].timestamp, 0);
    transState[pid].state = ABORTING;
    return true;
  }
  else
    return false;
}

/**************************************
 *   Standard Eager / Eager Methods   *
 **************************************/

/**
 * @ingroup transCoherence
 * @brief   eager eager read
 * 
 * @param pid   Process ID
 * @param tid   Thread ID
 * @param raddr Real address
 * @return Coherency status
 */
GCMRet transCoherence::readEE(int pid, int tid, RAddr raddr)
{
  RAddr caddr = addrToCacheLine(raddr);
  GCMRet retval = SUCCESS;

  map<RAddr, cacheState>::iterator it;
  it = permCache.find(caddr);

  //! If the cache line has been instantiated in our Map
  if(it != permCache.end()){
    struct cacheState per = it->second;
    if(per.writers.size() >= 1 && (per.writers.count(pid) != 1))
    {
      int nackPid = *per.writers.begin();

      Time_t nackTimestamp = transState[nackPid].timestamp;
      Time_t myTimestamp = transState[pid].timestamp;

      if(nackTimestamp <= myTimestamp && transState[pid].cycleFlag)
      {
        tmReport->reportNackLoad(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        tmReport->reportAbort(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        transState[pid].state = ABORTING;
        return ABORT;
      }

      if(nackTimestamp >= myTimestamp)
        transState[nackPid].cycleFlag = 1;

      tmReport->reportNackLoad(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
      transState[pid].state = NACKED;
      retval = NACK;
    }
    else{
      per.readers.insert(pid);
      tmReport->registerLoad(transState[pid].utid,transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = per;
      transState[pid].state = RUNNING;
      retval = SUCCESS;
    }
  }
  //! We haven't, so create a new one
  else{
      tmReport->registerLoad(transState[pid].utid, transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = newReadState(pid);
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }

  return retval;
}

/**
 * @ingroup transCoherence
 * @brief   eager eager write
 * 
 * @param pid   Process ID
 * @param tid   Thread ID
 * @param raddr Real address
 * @return Coherency status
 */
GCMRet transCoherence::writeEE(int pid, int tid, RAddr raddr)
{
  RAddr caddr = addrToCacheLine(raddr);
  GCMRet retval = SUCCESS;

  map<RAddr, cacheState>::iterator it;
  it = permCache.find(caddr);

  //! If the cache line has been instantiated in our Map
  if(it != permCache.end()){
    struct cacheState per = it->second;
    //! If there is more than one reader, or there is a single reader who happens not to be us
    if(per.readers.size() > 1 || ((per.readers.size() == 1) && (per.readers.count(pid) != 1)))
    {
      set<int>::iterator it = per.readers.begin();
      int nackPid = *it;
      //!  Grab the first reader than isn't us
      if(nackPid == pid)
      {
        ++it;
        nackPid = *it;
      }
      //!  Take our timestamp as well as the readers
      Time_t nackTimestamp = transState[nackPid].timestamp;
      Time_t myTimestamp = transState[pid].timestamp;

      //!  If the process that is going to nack us is older than us, and we have cycle flag set, abort
      if(nackTimestamp <= myTimestamp && transState[pid].cycleFlag)
      {
        tmReport->reportNackStore(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        tmReport->reportAbort(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        transState[pid].state = ABORTING;
        return ABORT;
      }

      //!  If we are older than the guy we're nacking on, then set her cycle flag to indicate possible deadlock
      if(nackTimestamp >= myTimestamp)
        transState[nackPid].cycleFlag = 1;

      tmReport->reportNackStore(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);

      transState[pid].state = NACKED;
      retval = NACK;
    }
    else if((per.writers.size() > 1) || ((per.writers.size() == 1) && (per.writers.count(pid) != 1)))
    {

      set<int>::iterator it = per.writers.begin();
      int nackPid = *it;

      //!  Grab the first reader than isn't us
      if(nackPid == pid)
      {
        ++it;
        nackPid = *it;
      }

      Time_t nackTimestamp = transState[nackPid].timestamp;
      Time_t myTimestamp = transState[pid].timestamp;

      if(nackTimestamp <= myTimestamp && transState[pid].cycleFlag)
      {
        tmReport->reportNackStore(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        tmReport->reportAbort(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
        transState[pid].state = ABORTING;
        return ABORT;
      }

      if(nackTimestamp >= myTimestamp)
        transState[nackPid].cycleFlag = 1;

      tmReport->reportNackStore(transState[pid].utid,pid, tid, nackPid, raddr, caddr, myTimestamp, nackTimestamp);
      transState[pid].state = NACKED;
      retval = NACK;
    }
    else{
      per.writers.insert(pid);
      tmReport->registerStore(transState[pid].utid, transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = per;
      transState[pid].state = RUNNING;
      retval = SUCCESS;
    }

  }
  //!  We haven't, so create a new one
  else{
      tmReport->registerStore(transState[pid].utid,transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = newWriteState(pid);
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }

  return retval;
}

/**
 * @ingroup transCoherence
 * @brief   eager eager begin
 * 
 * @param pid   Process ID
 * @param picode Instruction code
 * @return  Final coherency status
 */
GCMFinalRet transCoherence::beginEE(int pid, icode_ptr picode)
{
  struct GCMFinalRet retVal;

  //!  Subsume all nested transactions for now
  if(tmDepth[pid]>0)
  {
    //tmReport->registerBegin(transState[pid].utid,pid,tid,transState[pid].timestamp);
    tmDepth[pid]++;
    retVal.ret = IGNORE;
    //!  This is a subsumed begin, set BCFlag = 2
    retVal.BCFlag = 2;
    retVal.tuid = transState[pid].utid;
    return retVal;
  }
  else
  {
    //!  If we had just aborted, we need to now invalidate all the memory addresses we touched
    if(transState[pid].state == ABORTING)
    {
      map<RAddr, cacheState>::iterator it;
      for(it = permCache.begin(); it != permCache.end(); ++it)
      {
        it->second.writers.erase(pid);
        it->second.readers.erase(pid);
      }
      transState[pid].state = ABORTED;
      abortCount[pid]++;
    }

    //!  If we just finished an abort, its time to backoff
    if(transState[pid].state == ABORTED)
    {
      retVal.abortCount = abortCount[pid];
      retVal.ret = BACKOFF;
      transState[pid].state = RUNNING;
    }
    else
    {
      //!  Pass whether this is the begining of an aborted replay back to the context
      if(abortCount[pid]>0)
         retVal.BCFlag = 1;  //!  Replay
      else
        retVal.BCFlag = 0;

      transState[pid].timestamp = globalClock;
      transState[pid].beginPC = picode->addr;
      transState[pid].cycleFlag = 0;
      transState[pid].state = RUNNING;
      transState[pid].utid = transCoherence::utid++;

      tmDepth[pid]++;

      tmReport->registerBegin(transState[pid].utid,pid,picode->immed,picode->addr,transState[pid].timestamp);

      retVal.ret = SUCCESS;
      retVal.tuid = transState[pid].utid;
    }

	cyclesOnBegin[pid] = globalClock;
    return retVal;
  }
}

/**
 * @ingroup transCoherence
 * @brief   eager eager abort
 * 
 * @param pthread SESC pointer to thread
 * @param tid    Thread ID
 * @return Final coherency status
 */
struct GCMFinalRet transCoherence::abortEE(thread_ptr pthread, int tid)
{

  struct GCMFinalRet retVal;
  int pid = pthread->getPid();
  int writeSetSize = 0;
  transState[pid].timestamp = ((~0ULL) - 1024);
  transState[pid].beginPC = 0;
  stallCycle[pid] = 0;
  transState[pid].cycleFlag = 0;

  //!  We can't just decriment because we should be going back to the original begin, so tmDepth[pid] = 0
  tmDepth[pid]=0;

  map<RAddr, cacheState>::iterator it;
  for(it = permCache.begin(); it != permCache.end(); ++it)
    writeSetSize += it->second.writers.count(pid);

  retVal.writeSetSize = writeSetSize;

//   if((pthread->tmAbortMax < 0) || (pthread->tmAbortMax >= 0)&&(pthread->abortCount < pthread->tmAbortMax))
//   {
    transState[pid].state = ABORTING;    
    retVal.ret = SUCCESS;

	cyclesOnAbort[pid] += globalClock - cyclesOnBegin[pid];
    return retVal;
//   }
//   else{
//     retVal.ret = IGNORE;
//     return retVal;
//   }
}

/**
 * @ingroup transCoherence
 * @brief   eager eager commit
 * 
 * @param pid   Process ID
 * @param tid    Thread ID
 * @return Final coherency status
 */
struct GCMFinalRet transCoherence::commitEE(int pid, int tid)
{

  struct GCMFinalRet retVal;

  //!  Set the default BCFlag to 0, since the only other option for Commit is subsumed 2
  retVal.BCFlag = 0;

  if(tmDepth[pid]>1)
  {
    //tmReport->registerCommit(transState[pid].utid,pid,tid,transState[pid].timestamp); // Register Commit in Report
    tmDepth[pid]--;
    retVal.ret = IGNORE;
    //!  This commit is subsumed, set the BCFlag to 2
    retVal.BCFlag = 2;
    retVal.tuid = transState[pid].utid;
    return retVal;
  }
  else
  {
    //!  If we have already stalled for the commit, our state will be COMMITTING, Complete Commit
    if(transState[pid].state == COMMITTING)
    {
       tmReport->registerCommit(transState[pid].utid,pid,tid,transState[pid].timestamp); //!  Register Commit in Report

      int writeSetSize = 0;
      transState[pid].timestamp = ((~0ULL) - 1024);
      transState[pid].beginPC = 0;
      stallCycle[pid] = 0;
      transState[pid].cycleFlag = 0;
      abortCount[pid] = 0;
      tmDepth[pid] = 0;

      map<RAddr, cacheState>::iterator it;
      for(it = permCache.begin(); it != permCache.end(); ++it)
      {
        writeSetSize += it->second.writers.erase(pid);
        it->second.readers.erase(pid);
      }

      retVal.writeSetSize = writeSetSize;
      retVal.ret = SUCCESS;
      transState[pid].state = COMMITTED;
      retVal.tuid = transState[pid].utid;
	  cyclesOnCommit[pid] += globalClock - cyclesOnBegin[pid];
      return retVal;
    }
    else
    {
      int writeSetSize = 0;
      map<RAddr, cacheState>::iterator it;
      for(it = permCache.begin(); it != permCache.end(); ++it)
        writeSetSize += it->second.writers.count(pid);
      transState[pid].state = COMMITTING;
      retVal.writeSetSize = writeSetSize;
      retVal.ret = COMMIT_DELAY;
      retVal.tuid = transState[pid].utid;
      return retVal;
    }
  }

}


/**************************************
 *   Standard Lazy / Lazy Methods   *
 **************************************/

/*
  * The Read function is much simpler in the Lazy approach since 
  * we do not have to worry about conflict detection.  We always
  * permit accecss and simply record the information.
*/

/**
 * @ingroup transCoherence
 * @brief   lazy lazy read
 * 
 * @param pid   Process ID
 * @param tid    Thread ID
 * @param raddr  Real address
 * @return Coherency status
 */
GCMRet transCoherence::readLL(int pid, int tid, RAddr raddr)
{
  RAddr caddr = addrToCacheLine(raddr);
  GCMRet retval = SUCCESS;

  //!  If we have been forced to ABORT
  if(transState[pid].state == DOABORT)
  {
    tmReport->reportAbort(transState[pid].utid,pid, tid, abortReason[pid].first, abortReason[pid].second, abortReason[pid].second, transState[pid].timestamp, 0);
    transState[pid].state = ABORTING;
    return ABORT;
  }

  map<RAddr, cacheState>::iterator it;
  it = permCache.find(caddr);

  //!  If the cache line has been instantiated in our Map
  if(it != permCache.end()){
    struct cacheState per = it->second;

      per.readers.insert(pid);
      tmReport->registerLoad(transState[pid].utid, transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = per;
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }
  //!  We haven't, so create a new one
  else{
      tmReport->registerLoad(transState[pid].utid, transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = newReadState(pid);
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }

  return retval;
}


/*
  * The Write function is much simpler in the Lazy approach since 
  * we do not have to worry about conflict detection.  We always
  * permit accecss and simply record the information.
*/

/**
 * @ingroup transCoherence
 * @brief   lazy lazy write
 * 
 * @param pid   Process ID
 * @param tid    Thread ID
 * @param raddr Real address
 * @return Coherency status
 */
GCMRet transCoherence::writeLL(int pid, int tid, RAddr raddr)
{
  RAddr caddr = addrToCacheLine(raddr);
  GCMRet retval = SUCCESS;

  //!  If we have been forced to ABORT
  if(transState[pid].state == DOABORT)
  {
    tmReport->reportAbort(transState[pid].utid,pid, tid, abortReason[pid].first, abortReason[pid].second, abortReason[pid].second,transState[pid].timestamp, 0);
    transState[pid].state = ABORTING;
    return ABORT;
  }

  map<RAddr, cacheState>::iterator it;
  it = permCache.find(caddr);

  //!  If the cache line has been instantiated in our Map
  if(it != permCache.end()){
    struct cacheState per = it->second;
      per.writers.insert(pid);
      tmReport->registerStore(transState[pid].utid,transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = per;
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }
  //!  We haven't, so create a new one
  else{
      tmReport->registerStore(transState[pid].utid,transState[pid].beginPC,pid,tid,raddr,caddr,transState[pid].timestamp);
      permCache[caddr] = newWriteState(pid);
      transState[pid].state = RUNNING;
      retval = SUCCESS;
  }

  return retval;
}

/**
 * @ingroup transCoherence
 * @brief lazy lazy begin
 * 
 * @param pid   Process ID
 * @param picode SESC icode pointer
 * @return Final coherency status
 */
GCMFinalRet transCoherence::beginLL(int pid, icode_ptr picode)
{
  struct GCMFinalRet retVal;

  //!  Subsume all nested transactions for now
  if(tmDepth[pid]>0)
  {
    //tmReport->registerBegin(transState[pid].utid,pid,tid,transState[pid].timestamp);
    tmDepth[pid]++;
    retVal.ret = IGNORE;
    //!  This begin is subsumed, set the BCFlag to 2
    retVal.BCFlag = 2;
    retVal.tuid = transState[pid].utid;
    return retVal;
  }
  else
  {
    //!  If we had just aborted, we need to now invalidate all the memory addresses we touched
    if(transState[pid].state == ABORTING)
    {
      transState[pid].state = ABORTED;
      abortCount[pid]++;
    }

      //!  Pass whether this is the begining of an aborted replay back to the context
      if(abortCount[pid]>0)
         retVal.BCFlag = 1;  //!  Replay
      else
        retVal.BCFlag = 0;

      transState[pid].timestamp = globalClock;
      transState[pid].beginPC = picode->addr;
      transState[pid].cycleFlag = 0;
      transState[pid].state = RUNNING;
      transState[pid].utid = transCoherence::utid++;


      tmDepth[pid]++;

      tmReport->registerBegin(transState[pid].utid,pid,picode->immed,picode->addr,transState[pid].timestamp);

      retVal.ret = SUCCESS;
      retVal.tuid = transState[pid].utid;
	  cyclesOnBegin[pid] = globalClock;
    }

  return retVal;
}

/**
 * @ingroup transCoherence
 * @brief lazy lazy abort
 * 
 * @param pthread SESC thread pointer
 * @param tid    Thread ID
 * @return Final coherency status
 */
struct GCMFinalRet transCoherence::abortLL(thread_ptr pthread, int tid)
{

  struct GCMFinalRet retVal;

  int pid = pthread->getPid();
  int writeSetSize = 0;
  transState[pid].timestamp = ((~0ULL) - 1024);
  transState[pid].beginPC = 0;
  stallCycle[pid] = 0;
  transState[pid].cycleFlag = 0;

  //!  We can't just decriment because we should be going back to the original begin, so tmDepth[pid] = 0
  tmDepth[pid]=0;

  //!  Write set size doesn't matter for Lazy/Lazy abort
  retVal.writeSetSize = 0;

  transState[pid].state = ABORTING;    
  retVal.ret = SUCCESS;

  cyclesOnAbort[pid] += globalClock - cyclesOnBegin[pid];
  return retVal;


}

/**
 * @ingroup transCoherence
 * @brief lazy lazy commit
 * 
 * @param pid   Process ID
 * @param tid    Thread ID
 * @return Final coherency status
 */
struct GCMFinalRet transCoherence::commitLL(int pid, int tid)
{
  struct GCMFinalRet retVal;

  //!  Set BCFlag default to 0, since only other option is subsumed BCFlag = 2
  retVal.BCFlag = 0;

  //!  If we have been forced to ABORT
  if(transState[pid].state == DOABORT)
  {
    retVal.ret = ABORT;
    transState[pid].state = ABORTING;
    tmReport->reportAbort(transState[pid].utid,pid, tid, abortReason[pid].first, abortReason[pid].second, abortReason[pid].second,transState[pid].timestamp, 0);
    return retVal;
  }


  if(tmDepth[pid]>1)
  {
    //tmReport->registerCommit(transState[pid].utid,pid,tid,transState[pid].timestamp); // Register Commit in Report
    tmDepth[pid]--;
    retVal.ret = IGNORE;
    //!  This is a subsumed commit, set BCFlag = 2
    retVal.BCFlag = 2;
    retVal.tuid = transState[pid].utid;
    return retVal;
  }
  else
  {
    //!  If we have already stalled for the commit, our state will be COMMITTING, Complete Commit
    if(transState[pid].state == COMMITTING)
    {
       tmReport->registerCommit(transState[pid].utid,pid,tid,transState[pid].timestamp); //!  Register Commit in Report

      int writeSetSize = 0;
      int didWrite = 0;
      transState[pid].timestamp = ((~0ULL) - 1024);
      transState[pid].beginPC = 0;
      stallCycle[pid] = 0;
      transState[pid].cycleFlag = 0;
      abortCount[pid] = 0;
      tmDepth[pid] = 0;


      map<RAddr, cacheState>::iterator it;
      set<int>::iterator setIt;

      for(it = permCache.begin(); it != permCache.end(); ++it)
      {
        didWrite = it->second.writers.erase(pid);

        //!  If we have written to this address, we must abort everyone who read/wrote to it
        if(didWrite)
        {
          //!  Increase our write set
          writeSetSize++;
          //!  Abort all who wrote to this
          for(setIt = it->second.writers.begin(); setIt != it->second.writers.end(); ++setIt)
            if(*setIt != pid)
            {
              transState[*setIt].state = DOABORT;
              abortReason[*setIt].first =  pid;
              abortReason[*setIt].second = (RAddr)it->first;
            }
          //!  Abort all who read from this
          for(setIt = it->second.readers.begin(); setIt != it->second.readers.end(); ++setIt)
            if(*setIt != pid)
            {
              transState[*setIt].state = DOABORT;
              abortReason[*setIt].first =  pid;
              abortReason[*setIt].second = (RAddr)it->first;
            }

          it->second.writers.clear();
          it->second.readers.clear();
        }
        else
          it->second.readers.erase(pid);
      }

      currentCommitter = -1;  //!  Allow other transaction to commit again
      retVal.writeSetSize = writeSetSize;
      retVal.ret = SUCCESS;
      transState[pid].state = COMMITTED;
      retVal.tuid = transState[pid].utid;
  	  cyclesOnCommit[pid] += globalClock - cyclesOnBegin[pid];
      return retVal;
    }
    else if(currentCommitter >= 0)
    {
      retVal.ret = NACK;
      transState[pid].state = NACKED;
      tmReport->reportNackCommit(transState[pid].utid,pid, tid, currentCommitter, transState[pid].timestamp, transState[currentCommitter].timestamp);
      return retVal;
    }
    else
    {
      tmReport->reportNackCommitFN(transState[pid].utid,pid,tid,transState[pid].timestamp); //!  Register Commit in Report
      int writeSetSize = 0;
      currentCommitter = pid; //!  Stop other transactions from being able to commit
      map<RAddr, cacheState>::iterator it;
      for(it = permCache.begin(); it != permCache.end(); ++it)
        writeSetSize += it->second.writers.count(pid);
      transState[pid].state = COMMITTING;
      retVal.writeSetSize = writeSetSize;
      retVal.ret = COMMIT_DELAY;
      retVal.tuid = transState[pid].utid;
      return retVal;
    }
  }
}


