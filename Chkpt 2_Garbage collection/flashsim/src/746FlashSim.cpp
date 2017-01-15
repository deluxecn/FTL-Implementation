
/*
 * 746FlashSim.cpp - Flash translation layer simulator for CMU 18-/15-746
 *
 * This file contains the implementation of CMU's 18746/15746: Storage Systems
 * course project framework
 */

#include "746FlashSim.h"

/*
 * operator() - Function wrapper for Controller::ExecuteCommand()
 *
 * This function is implemented out side the header file since there are
 * circular dependencies.
 */
template <typename PageType>
void FlashSim::ExecCallBack<PageType>::operator()(OpCode operation,
                                        Address addr) const {
  controller_p->ExecuteCommand(operation, addr);
}

template class FlashSim::ExecCallBack<TEST_PAGE_TYPE>;
