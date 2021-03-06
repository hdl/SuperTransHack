/**
 * @file
 * @author  jpoe   <>, (C) 2008, 2009
 * @date    09/19/08
 * @brief   This is the interface for the TM cache manager.
 *
 * @section LICENSE
 * Copyright: See COPYING file that comes with this distribution
 *
 * @section DESCRIPTION
 * C++ Interface: transactionCache
 * Functional cache used to store transactional data. At the functional level, all models
 * operate similiar to L/L with an infinite size temporary cache. 
 */
/////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////

#ifndef TRANSACTION_CACHE
#define TRANSACTION_CACHE

#include <map>
#include "SescConf.h"

/**
 * @def     SWAP_WORD(X)
 * Endianness (word)
 */
#define SWAP_WORD(X) (((((unsigned int)(X)) >> 24) & 0x000000ff) | \
				((((unsigned int)(X)) >>  8) & 0x0000ff00) | \
							 ((((unsigned int)(X)) <<  8) & 0x00ff0000) | \
							 ((((unsigned int)(X)) << 24) & 0xff000000))

/**
 * @def     SWAP_SHORT(X)
 * Endianness (short)
 */
#define SWAP_SHORT(X) ( ((((unsigned short)X)& 0xff00) >> 8) | ((((unsigned short)X)& 0x00ff) << 8) )

using namespace std;

typedef int32_t IntRegValue;
typedef uintptr_t RAddr;
typedef struct icode *icode_ptr;
typedef class ThreadContext *thread_ptr;


/**
 * @ingroup transCache
 * @brief   Transaction Cache
 *
 * This class contains the methods and fields required
 * for storing memory information about references
 * inside of a transaction.
 */
class transactionCache
{
  public:
    /* Contructor */
    transactionCache();

    IntRegValue loadWord(RAddr addr);
    void storeWord(RAddr addr, IntRegValue value);
    void storeHalfWord(RAddr addr, IntRegValue value);
    void storeFPWord(RAddr addr, IntRegValue value);
    void storeDFP(RAddr addr, unsigned long long value);
    RAddr findWordAddress(RAddr addr);
    IntRegValue loadUnsignedHalfword(RAddr addr);
    IntRegValue loadHalfword(RAddr addr);
    IntRegValue loadByte(RAddr addr);
    float loadFPWord(RAddr addr);
    double loadDFP(RAddr addr);
    void storeByte(RAddr addr, IntRegValue value);
    void writeBuffer(char *buff,RAddr buffBegin, int count);
    void readBuffer(char *buff,RAddr buffBegin, int count);

    map<RAddr, IntRegValue>::iterator getBeginIterator();
    map<RAddr, IntRegValue>::iterator getEndIterator();

    /* Deconstructor */
    ~transactionCache();

  private:
     map<RAddr, IntRegValue>     memMap; //!< The Memory Map
};

#endif

/**
 * @typedef IntRegValue
 * int32_t.
 */

/**
 * @typedef RAddr
 * uintptr_t.
 */

/**
 * @typedef *thread_ptr
 * class ThreadContext
 */

/**
 * @typedef *icode_ptr
 * struct icode
 */


