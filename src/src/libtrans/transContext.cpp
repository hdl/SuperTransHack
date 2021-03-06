/**
 * @file
 * @author  jpoe   <>, (C) 2008, 2009
 * @date    09/19/08
 * @brief   This is the implementation for the transaction context module.
 *
 * @section LICENSE
 * Copyright: See COPYING file that comes with this distribution
 *
 * @section DESCRIPTION
 * C++ Implementation: transactionContext
 */
/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

#include <math.h>
#include "transContext.h"
#include "transReport.h"
#include "ThreadContext.h"
#include "transCoherence.h"
#include "opcodes.h"


/**
 * @ingroup transContext
 * @brief   Default constructor
 * 
 */
transactionContext::transactionContext()
{
  nackStallCycles = SescConf->getInt("TransactionalMemory","nackStallCycles");
  nackInstruction = NULL;
}

/**
 * @ingroup transContext
 * @brief   Constructor
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 */
transactionContext::transactionContext(thread_ptr pthread, icode_ptr picode)
{
  nackStallCycles = SescConf->getInt("TransactionalMemory","nackStallCycles");

  if( transGCM->getVersioning() == 0 )
  {
    abortBaseStallCycles = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
    abortVarStallCycles = SescConf->getInt("TransactionalMemory","secondaryVarStallCycles");
    commitBaseStallCycles = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
    commitVarStallCycles = SescConf->getInt("TransactionalMemory","primaryVarStallCycles");
  }
  else if (transGCM->getVersioning() == 1 )
  {
    abortBaseStallCycles = SescConf->getInt("TransactionalMemory","primaryBaseStallCycles");
    abortVarStallCycles = SescConf->getInt("TransactionalMemory","primaryVarStallCycles");
    commitBaseStallCycles = SescConf->getInt("TransactionalMemory","secondaryBaseStallCycles");
    commitVarStallCycles = SescConf->getInt("TransactionalMemory","secondaryVarStallCycles");
  }
  else
  {
    fprintf(stderr,"Unsupported Versioning Provided\n");
    exit(0);
  }

  abortExpBackoff = SescConf->getInt("TransactionalMemory","abortExpBackoff");
  abortLinBackoff = SescConf->getInt("TransactionalMemory","abortLinBackoff");
  applyRandomization = SescConf->getInt("TransactionalMemory","applyRandomization");

  nackInstruction=NULL;
  beginTransaction(pthread,picode);
}

/**
 * @ingroup transContext
 * @brief   Default destructor
 *
 */
transactionContext::~transactionContext()
{
}

/**
 * @ingroup transContext
 * @brief   Starts a transaction
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
*/
void transactionContext::beginTransaction(thread_ptr pthread, icode_ptr picode)
{

  GCMFinalRet retval = transGCM->begin(pthread->getPid(),picode);

  if(retval.ret == SUCCESS)
  {
    int i;
    this->pid = pthread->getPid();
    this->tid = picode->immed;
    this->tmBeginCode = picode;
    this->lo = pthread->lo;
    this->hi = pthread->hi;
    this->fcr0 = pthread->fcr0;
    this->fcr31 = pthread->fcr31;
    for(i = 0; i < 32; i++)
    {
      this->reg[i] = pthread->reg[i];
      this->fp[i] = pthread->fp[i];
    }
    this->reg[32] = pthread->reg[32];
    this->depth = pthread->getTMdepth();
    if(this->depth > 0)
      this->parent = pthread->transContext;
    else
      this->parent = NULL;

    pthread->transContext = this;

    pthread->incTMdepth();

    //! Set the BCFlag from retVal to indicate whether a Replay trans or not
    pthread->tmBCFlag = retval.BCFlag;
    pthread->tmTid = this->tid;

    //! Clear the aborting flag just in case it hasn't previously been cleared
    pthread->tmAborting = 0;

    //! Move instruction pointer to next instruction
    pthread->setPCIcode(picode->next);
  }
  else if (retval.ret == BACKOFF)
  {
    if(abortExpBackoff)
    {
      retval.abortCount = retval.abortCount % 15;
//       if (retval.abortCount > 10)
//         retval.abortCount = 10;
      stallInstruction(pthread,picode,((int)pow(abortExpBackoff,retval.abortCount)));
    }
    else
    {
      int abortStall = (rand()%abortLinBackoff + 1) * retval.abortCount;
      stallInstruction(pthread,picode,abortStall);
    }

      pthread->setPCIcode(nackInstruction);
      delete(this);
  }
  else if(retval.ret == IGNORE)
  {
    //! Set the BCFlag to the retVal version (in this case it should indicate subsumed)
    pthread->tmBCFlag = retval.BCFlag;
    pthread->setPCIcode(picode->next);
    delete(this);
  }
}

/**
 * @ingroup transContext
 * @brief   Aborts a transaction
 * 
 * @param pthread SESC thread pointer
 */
void transactionContext::abortTransaction(thread_ptr pthread)
{
  struct GCMFinalRet retVal = transGCM->abort(pthread,this->tid);

  if(retVal.ret == SUCCESS){
    int i;
    pthread->abortCount++;
    pthread->decTMdepth();

    pthread->fcr31 = this->fcr31;
    pthread->fcr0 = this->fcr0;
    pthread->lo = this->lo;
    pthread->hi = this->hi;

    for(i = 0; i < 32; i++){
      pthread->reg[i] = this->reg[i];
      pthread->fp[i] = this->reg[i];
    }
    pthread->reg[32] = this->reg[32];

    if(pthread->getTMdepth() > 0)
      pthread->transContext = this->parent;
    else
      pthread->transContext = NULL;

      createStall(pthread,getRndDelay(abortBaseStallCycles + (abortVarStallCycles * retVal.writeSetSize)));

      pthread->setPCIcode(tmBeginCode);

      pthread->tmAborting = 1;

      delete(this);
  }
  else{
      //pthread->setPCIcode(picode->next);
  }
}

/**
 * @ingroup transContext
 * @brief   Commits a transaction
 * 
 * @param pthread SESC thread pointer SESC thread pointer
 * @param picode Instruction code
 */
void transactionContext::commitTransaction(thread_ptr pthread, icode_ptr picode)
{
  struct GCMFinalRet retVal = transGCM->commit(this->pid, this->tid);

  //! We first delay during the commit
  if(retVal.ret == COMMIT_DELAY)
  {
    stallInstruction(pthread,picode,getRndDelay(commitBaseStallCycles + (commitVarStallCycles * retVal.writeSetSize)));
    pthread->setPCIcode(nackInstruction);
  }
  else if(retVal.ret == IGNORE)
  {
    pthread->tmBCFlag = retVal.BCFlag;
    pthread->setPCIcode(picode->next);
  }
  //! In the case of a Lazy model that can not commit yet
  else if(retVal.ret == NACK)
  {
      stallInstruction(pthread,picode,nackStallCycles);
      pthread->setPCIcode(nackInstruction);
  }
  //! In the case of a Lazy model where we are forced to Abort
  else if(retVal.ret == ABORT)
  {
    pthread->tmNacking = 0;
    abortTransaction(pthread);
  }
  //! If we have already delayed, go ahead and finalize commit in memory
  else
  {
    pthread->decTMdepth();

    map<RAddr, IntRegValue>::iterator begin = this->cacheGetBeginIterator();
    map<RAddr, IntRegValue>::iterator end = this->cacheGetEndIterator();

    ID(
        if(pthread->tmDebug == 0)
        {
      )
      for(; begin != end; begin++)
      {
          ID(
              if(pthread->tmDebugTrace)
              fprintf(tmReport->getOutfile(),
                      "<Trans> memDebg: %d  RELMEM %#10x -> %#10x\n",pthread->pid,
                      begin->first,begin->second);
            )
          *(unsigned int*) begin->first = begin->second;
      }
    ID(
        }
        else
        {
          for(; begin != end; begin++)
          {

            if(pthread->tmDebugTrace)
                fprintf(tmReport->getOutfile(),
                        "<Trans> memDebg: %d  RELMEM %#10x -> %#10x\tACTUAL: %#10x\n",pthread->pid,
                        begin->first,begin->second,*(unsigned int*)begin->first);
          }

        }
      )
    pthread->tmBCFlag = retVal.BCFlag;
    //!Move instruction pointer to next instruction
    pthread->setPCIcode(picode->next);

    if(pthread->tmDepth > 0)
      pthread->transContext = this->parent;

    delete(this);

  }
}

/**
 * @ingroup transContext
 * @brief   load word from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 */
void transactionContext::cacheLW(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);

  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setREG(picode, RT, this->cacheLW(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load unsigned half word from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 */
void transactionContext::cacheLUH(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setREG(picode, RT, this->cacheLUH(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load half word from TM cache
 * 
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 */
void transactionContext::cacheLHW(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setREG(picode, RT, this->cacheLHW(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load unsigned byte from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param raddr  Real address
 */
void transactionContext::cacheLUB(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setREG(picode, RT, this->cacheLUB(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load byte from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param raddr  Real address
 */
void transactionContext::cacheLB(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setREG(picode, RT, this->cacheLB(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load single prec floating point from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param raddr  Real address
 */
void transactionContext::cacheLWFP(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setFP(picode, ICODEFT, this->cacheLWFP(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   load double prec floating point from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
*/
void transactionContext::cacheLDFP(thread_ptr pthread, icode_ptr picode, RAddr raddr)
{
  GCMRet retval = transGCM->read(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      pthread->setDP(picode, RT, this->cacheLDFP(raddr));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}


/**
 * @ingroup transContext
 * @brief   store byte to TM cache
 * 
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 * @param value   Cache line value
 */
void transactionContext::cacheSB(thread_ptr pthread, icode_ptr picode, RAddr raddr, IntRegValue value)
{
  GCMRet retval = transGCM->write(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      this->cacheSB(raddr,pthread->getREG(picode, RT));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   store half word to TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 * @param value   Cache line value
*/
void transactionContext::cacheSHW(thread_ptr pthread, icode_ptr picode, RAddr raddr, IntRegValue value)
{
  GCMRet retval = transGCM->write(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      this->cacheSHW(raddr,SWAP_SHORT(pthread->getREG(picode, RT)));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   store word to TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 * @param value   Cache line value
*/
void transactionContext::cacheSW(thread_ptr pthread, icode_ptr picode, RAddr raddr, IntRegValue value)
{
  GCMRet retval = transGCM->write(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;      
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      this->cacheSW(raddr,SWAP_WORD(pthread->getREG(picode, RT)));
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   store single prec floating point to TM cache
 * 
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 * @param value   Cache line value
 */
void transactionContext::cacheSWFP(thread_ptr pthread, icode_ptr picode, RAddr raddr, IntRegValue value)
{
  GCMRet retval = transGCM->write(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      this->cacheSWFP(raddr, value);
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   store double prec floating point to TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode  Instruction code
 * @param raddr   Real address
 * @param value   Cache line value
*/
void transactionContext::cacheSDFP(thread_ptr pthread, icode_ptr picode, RAddr raddr, unsigned long long value)
{
  GCMRet retval = transGCM->write(this->pid,this->tid,raddr);
  switch(retval)
  {
    case NACK:
      pthread->tmNacking = 1;
      stallInstruction(pthread,picode,nackStallCycles);
      break;
    case ABORT:
      pthread->tmNacking = 0;
      abortTransaction(pthread);
      break;
    case IGNORE:
    case SUCCESS:
      pthread->tmNacking = 0;
      this->cacheSDFP(raddr,value);
      break;
    default:
      printf("Error, unhandled GCMRet Value!\n");
      exit(1);
      break;
  }
}

/**
 * @ingroup transContext
 * @brief   store entire buffer to TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param buff
 * @param buffBegin
 * @param count
*/
void transactionContext::cacheWriteBuffer(thread_ptr pthread, icode_ptr picode, char *buff, RAddr buffBegin, int count)
{
  int i,tmp;
  int bytes = count % 4;
  int words = (int)((count - bytes)/4);
  GCMRet retval;

  for(i = 0; i < words; i++)
  {
    retval = SUCCESS; //transGCM->read(this->pid,this->tid,buffBegin+4*i);
    if(retval == SUCCESS || retval == NACK)
    {
      tmp = this->cacheLW(buffBegin + 4*i);
      buff[4*i+3] = (unsigned char)(tmp >> 0) & 0xFF;
      buff[4*i+2] = (unsigned char)(tmp >> 8) & 0xFF;
      buff[4*i+1] = (unsigned char)(tmp >> 16) & 0xFF;
      buff[4*i+0] = (unsigned char)(tmp >> 24) & 0xFF;
    }
  }

  for(i = 0; i < bytes; i++)
  {
    GCMRet retval = SUCCESS; //transGCM->read(this->pid,this->tid,buffBegin+4*words+i);
    if(retval == SUCCESS || retval == NACK)
    {
      buff[4*words + i] = (unsigned char)this->cacheLB(buffBegin+4*words+i);
    }
  }
}

/**
 * @ingroup transContext
 * @brief   read entire buffer from TM cache
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param buff
 * @param buffBegin
 * @param count
*/
void transactionContext::cacheReadBuffer(thread_ptr pthread, icode_ptr picode, char *buff, RAddr buffBegin, int count)
{
  int i;
  int bytes = count % 4;
  int words = (int)((count - bytes)/4);
  GCMRet retval;

  for(i = 0; i < words; i++)
  {
    retval = SUCCESS;//transGCM->write(this->pid,this->tid,buffBegin+4*i);
    if(retval == SUCCESS || retval == NACK)
    {
      this->cacheSB(buffBegin + 4*i, buff[4*i]);
      this->cacheSB(buffBegin + 4*i + 1, buff[4*i + 1]);
      this->cacheSB(buffBegin + 4*i + 2, buff[4*i + 2]);
      this->cacheSB(buffBegin + 4*i + 3, buff[4*i + 3]);
    }
  }

  for(i = 0; i < bytes; i++)
  {
    GCMRet retval = SUCCESS; //transGCM->read(this->pid,this->tid,buffBegin+4*words+i);
    if(retval == SUCCESS || retval == NACK)
    {
      this->cacheSB(buffBegin+4*words + i,buff[4*words + i]);
    }
  }
}

/**
 * @ingroup transContext
 * @brief   Stalls the GCM for a given period
 *
 * @param pthread SESC thread pointer
 * @param picode Instruction code
 * @param stallLength Length of stall
 *
 * This method tells the GCM to stall for the next stallLength cycles as well as creates
 * a duplicate of the current instruction so that we will try it again after the stall
 * period
*/
void transactionContext::stallInstruction(thread_ptr pthread, icode_ptr picode, int stallLength)
{
  icode_ptr nextCode = new icode;

  createStall(pthread,stallLength);

  int i;

  /** @note
   * We are going to have to change the Next pointer of the real instruction since it will
   * automatically incremented by the default instruction handler.  To do this, we need to
   * create an identical copy of the instruction, and use this
  */

  nextCode->instID = picode->instID;
  nextCode->func = picode->func;
  nextCode->args[0] = picode->args[0];
  nextCode->args[1] = picode->args[1];
  nextCode->args[2] = picode->args[2];
  nextCode->args[3] = picode->args[3];
  nextCode->immed = picode->immed;
  nextCode->next = picode->next;
  nextCode->addr = picode->addr;
  nextCode->not_taken = picode->not_taken;
  nextCode->is_target = picode->is_target;
  nextCode->opnum = picode->opnum;
  nextCode->opflags = picode->opflags;
  nextCode->instr = picode->instr;
  nextCode->target = picode->target;

 if(this->nackInstruction != NULL)
   delete(this->nackInstruction);
  this->nackInstruction = nextCode;
}

/**
 * @ingroup transContext
 * @brief   Stalls a thread for a given period
 * 
 * @param pthread SESC thread pointer
 * @param stallLength Length of stall
 */
void transactionContext::createStall(thread_ptr pthread, int stallLength)
{
  transGCM->stallUntil(pthread->pid,stallLength);
}

/**
 * @ingroup transContext
 * @brief   Checks to see if the transaction is aborted
 *
 * @return  Is this transaction aborted?
 */
bool transactionContext::checkAbort()
{
  if(transGCM->checkAbort(this->pid, this->tid))
    return true;
  else
    return false;
}






