// -*-Mode: C++;-*-

// * BeginRiceCopyright *****************************************************
//
// $HeadURL$
// $Id$
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2017, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

/* 
 * File:   LocalTraceAnalyzer.cpp
 * Author: Lai Wei <lai.wei@rice.edu>
 * 
 * Created on March 6, 2018, 11:27 PM
 * 
 * Analyzes traces for a rank/thread and generates a summary temporal context tree.
 */

#include <string>
using std::string;
#include <vector>
using std::vector;

#include <lib/prof-lean/hpcrun-fmt.h>

#include "LocalTraceAnalyzer.hpp"

namespace TraceAnalysis {

  LocalTraceAnalyzer::LocalTraceAnalyzer(BinaryAnalyzer& binaryAnalyzer, 
          CCTVisitor& cctVisitor, string traceFileName, Time minTime) : 
          binaryAnalyzer(binaryAnalyzer), reader(cctVisitor, traceFileName, minTime) {}

  LocalTraceAnalyzer::~LocalTraceAnalyzer() {}
  
  static inline void pushOneNode(vector<TCTANode*>& activeStack, TCTANode* node, 
          Time startTimeExclusive, Time startTimeInclusive) {
    node->getTraceTime()->setStartTime(startTimeExclusive, startTimeInclusive);
    activeStack.push_back(node);
  }
  
  static inline TCTANode* popOneNode(vector<TCTANode*>& activeStack, 
          Time endTimeInclusive, Time endTimeExclusive) {
    TCTANode* node = activeStack.back();
    node->getTraceTime()->setEndTime(endTimeInclusive, endTimeExclusive);
    activeStack.pop_back();
    
    if (node->type == TCTANode::Iter || node->type == TCTANode::Func
            || node->type == TCTANode::Root) {
      bool hasDupChild = ((TCTATraceNode*)node)->hasDuplicateChild();
      if (hasDupChild) {
        TCTProfileNode* profile = TCTProfileNode::newProfileNode(node);
        delete node;
        node = profile;
      }
    }
    
    if (activeStack.size() > 0) activeStack.back()->addChild(node);
    return node;
  }
  
  static TCTANode* popActiveStack(vector<TCTANode*>& activeStack, 
          Time endTimeInclusive, Time endTimeExclusive) {
    if (activeStack.back()->type == TCTANode::Iter) {
      // When need to pop an iteration, first pop iteration and then pop the loop containing this iteration.
      popOneNode(activeStack, endTimeInclusive, endTimeExclusive);
    }
    return popOneNode(activeStack, endTimeInclusive, endTimeExclusive);
  }
  
  static void pushActiveStack(vector<TCTANode*>& activeStack, TCTANode* node, 
          Time startTimeExclusive, Time startTimeInclusive) {
    // Iteration Detection
    if (activeStack.back()->type == TCTANode::Iter) {
      TCTIterationTraceNode* iter = (TCTIterationTraceNode*) activeStack.back();
      CFGAGraph* cfg = iter->cfgGraph;
      int prevRA = 0;
      for (int i = iter->getNumChild()-1; i >= 0 && !cfg->hasChild(prevRA); i--)
        prevRA = iter->getChild(i)->ra;
      
      // when both prevRA and thisRA is child of cfg, and thisRA is not a successor of prevRA
      if (cfg->hasChild(prevRA) && cfg->hasChild(node->ra) && cfg->compareChild(prevRA, node->ra) != -1) {
        // pop the old iteration
        popOneNode(activeStack, startTimeExclusive, startTimeInclusive);
        // push a new iteration
        int iterNum = 0;
        if (activeStack.back()->type == TCTANode::Loop)
          iterNum = ((TCTLoopTraceNode*)activeStack.back())->getNumChild();
        pushOneNode(activeStack, new TCTIterationTraceNode(activeStack.back()->id, iterNum, activeStack.size(), activeStack.back()->cfgGraph),
                startTimeExclusive, startTimeInclusive);
      }
    }
    
    if (node->type == TCTANode::Loop) {
      // for loops with valid cfgGraph
      if (node->cfgGraph != NULL) {
        //TODO: if the parent is func or iter node, check if if this loop is "split".
        if (activeStack.back()->type == TCTANode::Iter || activeStack.back()->type == TCTANode::Func) {
          TCTATraceNode* parent = (TCTATraceNode*)activeStack.back();
          for (int i = parent->getNumChild()-1; i >= 0; i--)
            if (parent->getChild(i)->id == node->id) {
              // if the loop is split, re-push the loop and its last iteration onto stack.
              // Temporarily remove them from their parent (as they will be re-added later when popped from stack).
              TCTLoopTraceNode* loop = (TCTLoopTraceNode*)parent->removeChild(i);
              TCTANode* iter = loop->removeChild(loop->getNumChild()-1);
              pushOneNode(activeStack, loop, loop->getTraceTime()->getStartTimeExclusive(), loop->getTraceTime()->getStartTimeInclusive());
              pushOneNode(activeStack, iter, iter->getTraceTime()->getStartTimeExclusive(), iter->getTraceTime()->getStartTimeInclusive());
              
              // remove parent's children starting from i, add them as children to the loop.
              while (parent->getNumChild() > i) {
                TCTANode* child = parent->removeChild(i);
                pushActiveStack(activeStack, child, child->getTraceTime()->getStartTimeExclusive(), child->getTraceTime()->getStartTimeInclusive());
                popActiveStack(activeStack, child->getTraceTime()->getEndTimeInclusive(), child->getTraceTime()->getEndTimeExclusive());
              }

              // loop and iteration now on stack, delete node and return
              delete node;
              return;
            }
        }
        
        // if the loop is not "split", add to stack
        pushOneNode(activeStack, node, startTimeExclusive, startTimeInclusive);
        // create a iteration
        pushOneNode(activeStack, new TCTIterationTraceNode(activeStack.back()->id, 0, activeStack.size(), activeStack.back()->cfgGraph),
                startTimeExclusive, startTimeInclusive);
        return;
      } else {
        // for loops without valid cfgGraph, turn them into profile nodes.
        TCTProfileNode* profile = TCTProfileNode::newProfileNode(node);
        delete node;
        pushOneNode(activeStack, profile, startTimeExclusive, startTimeInclusive);
        return;
      }
    }
    
    pushOneNode(activeStack, node, startTimeExclusive, startTimeInclusive);
  }

  int computeLCADepth(CallPathSample* prev, CallPathSample* current) {
    int depth = current->getDepth();
    int dLCA = current->dLCA;
    if (dLCA != HPCRUN_FMT_DLCA_NULL) {
      while (depth > 0 && dLCA > 0) {
        if (current->getFrameAtDepth(depth).type == CallPathFrame::Func)
          dLCA--;
        depth--;
      }
    }
    
    if (depth == 0) depth = current->getDepth();
    
    if (depth > prev->getDepth()) depth = prev->getDepth();
    while (depth > 0 && prev->getFrameAtDepth(depth).id != current->getFrameAtDepth(depth).id)
      depth--;
    
    if (depth == 0) {
      printf("LCA Depth = 0:\n");
      printf("Prev = ");
      for (int i = 0; i < prev->getDepth(); i++)
        printf(" %s, ", prev->getFrameAtDepth(i).name.c_str());
      printf("\nCurr = ");
      for (int i = 0; i < current->getDepth(); i++)
        printf(" %s, ", current->getFrameAtDepth(i).name.c_str());
      printf(" dLCA = %u.\n", current->dLCA);
    }
    
    return depth;
  }
  
  void LocalTraceAnalyzer::analyze() {
    // Init and process first sample
    vector<TCTANode*> activeStack;
    CallPathSample* current = reader.readNextSample();
    uint64_t numSamples = 0;
    if (current != NULL) {
      numSamples++;
      for (int i = 0; i <= current->getDepth(); i++) {
        CallPathFrame& frame = current->getFrameAtDepth(i);
        if (frame.type == CallPathFrame::Root) {
          TCTANode* node = new TCTRootNode(frame.id, frame.name, activeStack.size());
          pushOneNode(activeStack, node, 0, current->timestamp);
        } 
        else if (frame.type == CallPathFrame::Loop) {
          TCTANode* node = new TCTLoopTraceNode(frame.id, frame.name, activeStack.size(), 
                  binaryAnalyzer.findLoop(frame.vma));
          pushActiveStack(activeStack, node, 0, current->timestamp);
        }
        else {
          // frame.type == CallPathFrame::Func
          TCTANode* node = new TCTFunctionTraceNode(frame.id, frame.name, activeStack.size(),
                  binaryAnalyzer.findFunc(frame.vma), frame.ra);
          pushActiveStack(activeStack, node, 0, current->timestamp);
        }
      }
    }
    
    // Process later samples
    CallPathSample* prev = current;
    current = reader.readNextSample();
    while (current != NULL) {
      numSamples++;
      int lcaDepth = computeLCADepth(prev, current);
      int prevDepth = prev->getDepth();
      
      while (prevDepth > lcaDepth) {
        prevDepth--;
        popActiveStack(activeStack, prev->timestamp, current->timestamp);
      }
      
      for (int i = lcaDepth + 1; i <= current->getDepth(); i++) {
        CallPathFrame& frame = current->getFrameAtDepth(i);
        if (frame.type == CallPathFrame::Loop) {
          TCTANode* node = new TCTLoopTraceNode(frame.id, frame.name, activeStack.size(), 
                  binaryAnalyzer.findLoop(frame.vma));
          pushActiveStack(activeStack, node, prev->timestamp, current->timestamp);
        } else {
          // frame.type == CallPathFrame::Func
          TCTANode* node = new TCTFunctionTraceNode(frame.id, frame.name, activeStack.size(),
                  binaryAnalyzer.findFunc(frame.vma), frame.ra);
          pushActiveStack(activeStack, node, prev->timestamp, current->timestamp);
        }
      }
      
      delete prev;
      prev = current;
      current = reader.readNextSample();
    }
    
    TCTANode* root = NULL;
    
    // Pop every thing in active stack.
    while (activeStack.size() > 0)
      root = popActiveStack(activeStack, prev->timestamp, prev->timestamp + 1);
    delete prev;
    
    printf("\n%s", root->toString(10, 0).c_str());
    
    //TCTProfileNode* profile = TCTProfileNode::newProfileNode(root);
    delete root;
    //printf("\n%s", profile->toString(10, 0).c_str());
    //delete profile;
  }
}
