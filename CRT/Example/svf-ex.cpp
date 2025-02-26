//===- svf-ex.cpp -- A driver example of SVF-------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===-----------------------------------------------------------------------===//

/*
 // A driver program of SVF including usages of SVF APIs
 //
 // Author: Yulei Sui,
 */
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Analysis/ValueTracking.h>
#include "llvm/ADT/APInt.h"
#include "llvm/IR/BasicBlock.h"
//#include "llvm/IR/IRBuilder.h"
//#include "llvm/ADT/APFloat.h"
//#include "llvm/ADT/SmallVector.h"
#include "SVF-FE/LLVMUtil.h"
#include "Graphs/SVFG.h"
#include "WPA/Andersen.h"
#include "WPA/FlowSensitiveTBHC.h"
#include "SABER/LeakChecker.h"
#include "SVF-FE/PAGBuilder.h"
#include "MemoryModel/MemModel.h"
#include "SVF-FE/SymbolTableInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/InstrTypes.h"
#include <iostream>
#include <fstream>

#if 0
#include "llvm/Transforms/IPO/StripSymbols.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/TypeFinder.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Local.h"
#endif
#include <iostream>
#include <ctime>
#include <unistd.h>
#include <queue>
void updateBC();
using namespace std;

std::string gen_random(const int len) {

    std::string tmp_s;
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    srand( (unsigned) time(NULL) * getpid());

    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i)
        tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];


    return tmp_s;

}


using namespace SVF;
using namespace llvm;
using namespace std;
Value * createGlobalPointer(Module * mod, Type * ty, string name);
Value * createGlobal(Module * mod, Type * ty, string name);

void dumper(Value * val) {
		cerr << "***************************************************" <<endl;
		val->dump();
		cerr<< "****************************************************" <<endl;
}
typedef struct e{
        Value* val;
        Function * fun;
    } VFPair;

vector<VFPair *> vfp;
vector<Value *> vec;
Type * qtype;


static llvm::cl::opt<std::string> InputFilename(cl::Positional,
        llvm::cl::desc("<input bitcode>"), llvm::cl::init("-"), cl::Required);

static llvm::cl::opt<std::string> KernFuncs(cl::Positional, llvm::cl::desc("<kernel functions>"), cl::Required);
static llvm::cl::opt<std::string> UserFuncs(cl::Positional, llvm::cl::desc("<user functions>"), cl::Required);
static llvm::cl::opt<std::string> SafeFuncs(cl::Positional, llvm::cl::desc("<safelist functions>"), cl::Required);
static llvm::cl::opt<std::string> TaskCreateFuncs(cl::Positional, llvm::cl::desc("<Task Create functions>"), cl::Required);
static llvm::cl::opt<std::string> IOMAP(cl::Positional, llvm::cl::desc("<IO Map for the device compiled"), cl::Required);

static llvm::cl::opt<bool> LEAKCHECKER("leak", llvm::cl::init(false),
                                       llvm::cl::desc("Memory Leak Detection"));
static llvm::cl::opt<bool> PRINT_PTS_TO("pts_to", llvm::cl::init(false),
                                       llvm::cl::desc("Points to Kernel Values?"));

static llvm::cl::opt<bool> DUMP("dump", llvm::cl::init(false),
									   llvm::cl::desc("Dump offending values?"));



/*!
 * An example to query alias results of two LLVM values
 */
AliasResult aliasQuery(PointerAnalysis* pta, Value* v1, Value* v2){
	return pta->alias(v1,v2);
}

/*!
 * An example to print points-to set of an LLVM value
 */
std::string printPts(PointerAnalysis* pta, Value* val){

    std::string str;
    raw_string_ostream rawstr(str);

    NodeID pNodeId = pta->getPAG()->getValueNode(val);
    const NodeBS& pts = pta->getPts(pNodeId);
    for (NodeBS::iterator ii = pts.begin(), ie = pts.end();
            ii != ie; ii++) {
        rawstr << " " << *ii << " ";
        PAGNode* targetObj = pta->getPAG()->getPAGNode(*ii);
        if(targetObj->hasValue()){
            rawstr << "(" <<*targetObj->getValue() << ")\t ";
        }
    }

    return rawstr.str();

}


/*!
 * An example to query/collect all successor nodes from a ICFGNode (iNode) along control-flow graph (ICFG)
 */
void traverseOnICFG(ICFG* icfg, const Instruction* inst){
	ICFGNode* iNode = icfg->getBlockICFGNode(inst);
	FIFOWorkList<const ICFGNode*> worklist;
	std::set<const ICFGNode*> visited;
	worklist.push(iNode);

	/// Traverse along VFG
	while (!worklist.empty()) {
		const ICFGNode* vNode = worklist.pop();
		for (ICFGNode::const_iterator it = iNode->OutEdgeBegin(), eit =
				iNode->OutEdgeEnd(); it != eit; ++it) {
			ICFGEdge* edge = *it;
			ICFGNode* succNode = edge->getDstNode();
			if (visited.find(succNode) == visited.end()) {
				visited.insert(succNode);
				worklist.push(succNode);
			}
		}
	}
}

/*!
 * An example to query/collect all the uses of a definition of a value along value-flow graph (VFG)
 */
void traverseOnVFG(const SVFG* vfg, Value* val){
	PAG* pag = PAG::getPAG();

    PAGNode* pNode = pag->getPAGNode(pag->getValueNode(val));
    const VFGNode* vNode = vfg->getDefSVFGNode(pNode);
    FIFOWorkList<const VFGNode*> worklist;
    std::set<const VFGNode*> visited;
    worklist.push(vNode);

	/// Traverse along VFG
	while (!worklist.empty()) {
		const VFGNode* vNode = worklist.pop();
		for (VFGNode::const_iterator it = vNode->OutEdgeBegin(), eit =
				vNode->OutEdgeEnd(); it != eit; ++it) {
			VFGEdge* edge = *it;
			VFGNode* succNode = edge->getDstNode();
			if (visited.find(succNode) == visited.end()) {
				visited.insert(succNode);
				worklist.push(succNode);
			}
		}
	}

    /// Collect all LLVM Values
    for(std::set<const VFGNode*>::const_iterator it = visited.begin(), eit = visited.end(); it!=eit; ++it){
    	const VFGNode* node = *it;
    //SVFUtil::outs() << *node << "\n";
        /// can only query VFGNode involving top-level pointers (starting with % or @ in LLVM IR)
        //if(!SVFUtil::isa<MRSVFGNode>(node)){
        //    const PAGNode* pNode = vfg->getLHSTopLevPtr(node);
        //    const Value* val = pNode->getValue();
        //}
    }
}

void streamToVec(ifstream& infile, vector<string>& vec) {
	std::string line;
	while (std::getline(infile, line))
	{
    		vec.push_back(line); 
	}
}

void printValsInFun(Function * fun) {
	for (inst_iterator I = inst_begin(*fun), J = inst_end(*fun); I != J; ++I) {
                                const Instruction *inst = &*I;
                                for (auto iter = inst->op_begin(); iter != inst->op_end(); ++iter) {
					if (!iter->get()->getName().empty())
                                        cerr << iter->get()->getName().data() << endl;
                                }
                        }
}
void pushValsInFun(Function * fun, vector<Value *>& vec, PAG *pag, vector<VFPair*> *vfp) {
        for (inst_iterator I = inst_begin(*fun), J = inst_end(*fun); I != J; ++I) {
                                const Instruction *inst = &*I;
                                for (auto iter = inst->op_begin(); iter != inst->op_end(); ++iter) {
					if (pag->hasValueNode(iter->get())) {
						//cerr<<"Pushing" << iter->get()->getName().data() <<endl;
                                        	vec.push_back((iter->get()));
											if(vfp){
												VFPair * pair = new VFPair;
												pair->val = iter->get(); pair->fun = fun;
												vfp->push_back(pair);
											}
					}	
                                }
                        }
}
string intrinsics[] = {"llvm.trap", "llvm.dbg.declare"};
bool isIntrinsic(string target) {
		for (int i =0; i< (sizeof(intrinsics)/sizeof(intrinsics[0])); i++) {
				if(intrinsics[i].compare(target) == 0)
						return true;
		}
		return false;
}
/* */
#define vContains(v, arg) (std::find(v.begin(), v.end(), arg) != v.end())
vector<Value *> uservalues;
std::vector<std::string> moduleNameVec;
vector<string> kernFuncs;
vector<string> userFuncs;
vector<string> safeFuncs;
map<string,int> creators;
typedef struct {
		string name;
		string driver;
		unsigned int size;
} desc;
map<unsigned int,desc> svdmap;
void parseArguments(int argc, char ** argv) {
	int arg_num = 0;
    int error_value = 0;
    char **arg_value = new char*[argc];
    SVFUtil::processArguments(argc, argv, arg_num, arg_value, moduleNameVec);
    cl::ParseCommandLineOptions(arg_num, arg_value,
                                "Whole Program Points-to Analysis\n");

	ifstream kernFuncsFile(argv[KernFuncs.getPosition()]);
    streamToVec(kernFuncsFile, kernFuncs);
#if 0
    std::vector<string> kernFuncs((std::istreambuf_iterator<string>(kernFuncsFile)),
    std::istreambuf_iterator<string>());
#endif
    ifstream userFuncsFile(argv[UserFuncs.getPosition()]);
    streamToVec(userFuncsFile, userFuncs);


    ifstream safeFuncsFile(argv[SafeFuncs.getPosition()]);
    streamToVec(safeFuncsFile, safeFuncs);

    ifstream createFuncsFile(argv[TaskCreateFuncs.getPosition()]);
    vector<string> createFuncs;
    streamToVec(createFuncsFile, createFuncs);


    for (auto s: createFuncs) {
            std::string delimiter = ":";
            std::string name = s.substr(0, s.find(delimiter));
            string arg = s.substr(s.find(delimiter) + 1, s.size());
            int argi = stoi(arg, 0);
            creators.insert(pair<string, int>(name,argi));
    }

	ifstream iomapFile(argv[IOMAP.getPosition()]);
	desc temp;
	unsigned int addr;
	while(iomapFile >> temp.name)
	{
		iomapFile >> addr;
		iomapFile >> temp.size;
		iomapFile >> temp.driver;
		svdmap[addr] = temp;
	}

	cout << "*********************************" <<endl;
#if 0
	for (pair<int, desc> p : svdmap) {
			cout << std::hex << p.first << p.second.name << p.second.size <<endl;
	}
#endif 

	//REmove the functions that are supposed to be interfaces for the kernel. We could have local variables that
    //may alias to the local pointers in that function
    for (auto func: safeFuncs) {
        kernFuncs.erase(std::remove(kernFuncs.begin(), kernFuncs.end(), func), kernFuncs.end());
    }

}
SVFModule* svfModule;
PAG *pag;
Andersen *ander;
FlowSensitive *fspta;
llvm::Module * ll_mod;
void buildPTA() {
	svfModule = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(moduleNameVec);
    PAGBuilder builder;
    pag = builder.build(svfModule);
	/// Create Andersen's pointer analysis
    //ander = AndersenWaveDiff::createAndersenWaveDiff(pag); TODO: This threw error will uncomment later
    //Create FlowSensitiveTBHC
    //FlowSensitiveTBHC fspta= FlowSensitiveTBHC(pag);
    static FlowSensitive fsptal = FlowSensitive(pag);
#if 0 // TODO: 
    fsptal.initialize();
    fsptal.analyze();
	fspta = &fsptal;
#endif 
	SVFModule::llvm_iterator F = svfModule->llvmFunBegin();
	Function *fun = *F;
	ll_mod = fun->getParent();

}
void printDI(Instruction * instruction) {
		const llvm::DebugLoc &debugInfo = instruction->getDebugLoc();
						if (debugInfo) {
                        StringRef directory = debugInfo->getDirectory();
                        StringRef filePath = debugInfo->getFilename();
                        int line = debugInfo->getLine();
                        int column = debugInfo->getColumn();

                        cerr<<"Error: "<<directory.str()  << "/"<<filePath.str() << ":" << line << ":" << column<<endl;
						}
						else {
								instruction->dump();
						}
}
int taskKernelVoilations() {
	int error_value = 0;
	vector<VFPair *> vfp;
    vector<Value *> vec;
	/* Find Kernel Objects */
	for (SVFModule::llvm_iterator F = svfModule->llvmFunBegin(), E = svfModule->llvmFunEnd(); F != E; ++F) {
        //const SVFFunction* fun = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(*F);       
        Function *fun = *F;
        Value * val = (Value *)fun;
#if 0
        if (fun->hasFnAttribute("nonnull")) {
                    cerr << "Function "<< val->getName().str() <<" has param";
                    
                    };
#endif
        for(auto arg = fun->arg_begin(); arg != fun->arg_end(); ++arg) {
#if 0
                if (val->getName().str().compare("xTaskCreate")==0) {
                    //if(F.hasFnAttribute("uservalue")) {
                    cerr << "Function args";
                    arg->dump();
                    //}
                }
#endif
            if (arg->hasAttribute(Attribute::UserValue)) {
                    uservalues.push_back(arg);
            }
        }
        if (vContains(kernFuncs, val->getName().data())) {
            pushValsInFun(fun,vec,pag, &vfp);
        }
    }

	/* Find user values */
	for (SVFModule::llvm_iterator F = svfModule->llvmFunBegin(), E = svfModule->llvmFunEnd(); F != E; ++F) {
        auto firstValid = true;
        if (vContains(userFuncs, (*F)->getName().data())) {
            auto funcName = (*F)->getName().str();
            vector<Value *> userVals;
            pushValsInFun(*F,userVals, pag, NULL);
            map<string, map<string,bool>> err_map;
            map<Value *, map<Value*,Function *>> err_map_val;
            for (auto &userValue : userVals) {
                for (auto &kernValue: vfp) {
#if 0
                    cerr << "Query to SVF:" << endl;
                    cerr << userValue->getName().data()<<endl;
                    cerr << kernValue->getName().data()<<endl;
#endif
                    if (aliasQuery(fspta, userValue, kernValue->val)) {
                        //cerr << "Error: In Function " << val->getName().data() << endl;
                        //cerr<<"Error: Task accessed Kernel Object"<<endl;
                        //if (err_map.contains (userValue->getName().str())) {
                            err_map[userValue->getName().str()].insert(pair<string, bool>(kernValue->val->getName().str(), true));
                            err_map_val[userValue].insert(pair<Value*, Function*>(kernValue->val, kernValue->fun));
                        //}
                        //else{
                        //  err_map.insert(pair<string, map<string,bool>> (userValue->getName().str())) ;
                        //}
                        //cerr << userValue->getName().data()<<" points to "
                        //<< kernValue->getName().data() <<endl;
                    }
                }
            }

#if 0
            for(auto val: err_map) {
                cerr << val.first << " points to:" <<endl;
                for (auto to: val.second) {
                    cerr << to.first << endl;
                }
            }
#endif
            for(auto val: err_map_val) {
                /* Rule out all the explicit voilation i.e. safe values */
                auto safevalue = false;
                for (auto to: val.second) {
                    if (vContains(uservalues, to.first)){
                            safevalue = true;
                            break;
                    }
                }
                if(safevalue) continue;
                if (auto globalVal= dyn_cast<llvm::GlobalVariable>(val.first)) {
                    //cout<<"Global Variable Type:";
                    if ((globalVal->getName().data())[0] == '.'){
                        cout<<"Hard Coded String, OK to skip"<<endl;
                        continue;
                    }

                }
				//cerr<< "Bypassing the gep"<<endl;
                if (auto gep = dyn_cast<llvm::GetElementPtrInst>(val.first)) {
                    //cout<<"GEP Type: POinter Val:";
                    /* TODO: We'll have to verify that all uses are contained within the string */
                    if ((gep->getPointerOperand()->getName().data())[0]=='.') {
                            //cout<<"Hard Coded String, OK to skip"<<endl;
                            continue;
                    }
					else {
					}
                }

				if (auto func = dyn_cast<Function>(val.first)) {
					if (vContains(safeFuncs, func->getName().str())) {
						//User intrinsics and safe functions as much as you want
						continue;
					}
				}

                if (isIntrinsic(val.first->getName().str())) {
                        continue;
                }

				if (auto alloca = dyn_cast<llvm::AllocaInst>(val.first)) {
						//Local variables are allowed to alias, this usually happens
						//when we have local variables passed to kernel APIs
						continue;
				}
				if (auto gep = dyn_cast<llvm::GetElementPtrInst>(val.first)) {
						if (auto c = dyn_cast<llvm::Constant>(gep->getPointerOperand())) {
								//If pointer was constant, it is to a hardcoded string array
								//We can bypass this.
								continue;
						}
				}

				cerr<<"User Function:" << funcName << " has the following ";
                error_value = -1;
                cerr<<"conflicting Symbols:" <<endl;
                if (DUMP)
						dumper(val.first);
                //if (val.first->hasName())
                //cerr << val.first->getName().str();
                //else
                //cerr << "Val of type: " ;//<< *val.first->getType();
                    //cerr <<"Purdue Ple"<<endif;
                if (Instruction *instruction = dyn_cast<Instruction>(val.first)) {
                        const llvm::DebugLoc &debugInfo = instruction->getDebugLoc();
						if (debugInfo.getAsMDNode())	{
                        StringRef directory = debugInfo->getDirectory();
                        StringRef filePath = debugInfo->getFilename();
                        int line = debugInfo->getLine();
                        int column = debugInfo->getColumn();
                        cerr<<"Error: "<<directory.str()  << "/"<<filePath.str() << ":" << line << ":" << column<<endl;
						}
						else {
								cerr<<"Fauly instruction with no DI:"; 
								dumper(instruction);
						}
                    }
                else if (auto global = dyn_cast<GlobalVariable>(val.first)) {
                        //cerr<<"No info for global"<<endl;
                        //SmallVector<DIGlobalVariableExpression *,10> vec;
                        //SmallVector<DIGlobalVariableExpression *, 1> vec;
                        llvm::SmallVector<DIGlobalVariableExpression *, 1> vec;
                        //SmallVectorImpl<DIGlobalVariableExpression *> vec;
                        //auto vec;
                        global->getDebugInfo(vec);
						int emitted= 0;

                        for (auto ve: vec) {
                                //ve->dump();
                                auto di = ve->getVariable();
                                cerr<<"Error: "<< di->getDirectory().str() <<"/"<< di->getFilename().str()<< ":" << di->getLine() <<endl;
								emitted =1;
                        }
						if (!emitted) {
							cerr<<"GV without DI:"; 
							global->dump();
						}
                }
				else if (auto func = dyn_cast<Function>(val.first)) {
						if (!vContains(safeFuncs, func->getName().str())) {
							cerr<<"Kernel Function called:";
							cerr<<func->getName().str()<<endl;
						}
				}
				else {
                        cerr<<"No information or pretty print for value"<<endl;
						dumper(val.first);
                }


                if (PRINT_PTS_TO) {
                cerr<<" points to :" <<endl;
                for (auto to: val.second) {
                    if(DUMP)
                        dumper(to.first);
                    if (Instruction *instruction = dyn_cast<Instruction>(to.first)) {
                        const llvm::DebugLoc &debugInfo = instruction->getDebugLoc();
                        StringRef directory = debugInfo->getDirectory();
                        StringRef filePath = debugInfo->getFilename();
                        int line = debugInfo->getLine();
                        int column = debugInfo->getColumn();
                        cerr<<"Error: "<<directory.str()  << filePath.str().erase(0,1) << ":" << line << ":" << column<<endl;
                    }

                    cerr<<"in Function:" << to.second->getName().str()<<endl;
#if 0
                    cerr<<" with users:" <<endl;
                    for(auto user: to.first->users())
                        user->dump();
#endif
                    //cerr<<endl;
                    //cerr << endl<<endl<<endl<<endl<<endl;
                }
                }
            }
        }
    }

	return 0;
}
typedef struct tcb {
		string name;
		Function * func;
		vector<Value *> objects;
} TCB;
vector<tcb> tasks;
string SEND_FUNC = "xQueueSend";
/* Value(Global Object)-New queue */
map<Value *, Value *> qmap;
int taskTaskVoilations() {
	int error_value =0;
	for (SVFModule::llvm_iterator F = svfModule->llvmFunBegin(), E = svfModule->llvmFunEnd(); F != E; ++F) {
        //const SVFFunction* fun = LLVMModuleSet::getLLVMModuleSet()->getSVFFunction(*F);
        Function *fun = *F;
        Value * val = (Value *)fun;
		//cout << (*F)->getName().str()<<endl;
		auto name = (*F)->getName().data();
		/* Get all tasks */
		if(creators.count(name)) {
			/* Find all callers of this function */
			for(auto U : val->users()){  
					for(auto op: U->operand_values())
							/* Here we can explicitly get the function 
							   using the argument information, however instead 
							   let's use a heuristic that only functions can 
							   be passed taskcreation APIs as task, so probably 
							   that would be the task function, in operand list  
							   one could be creator itself */
							if (auto task= dyn_cast<llvm::Function>(op)) {
								if(task->getName().str() == name)
										continue;

								TCB tcb;
								tcb.name = task->getName().str();
								tcb.func = task;
#if 0
								for (auto bb=task->begin();bb!=task->end();bb++) {
										for (auto stmt =bb->begin();stmt!=bb->end(); stmt++) {
												cout<<stmt->getOpcodeName()<<endl;
										}
								}
#endif
								pushValsInFun(task, tcb.objects,pag,NULL);
								tasks.push_back(tcb);	
							}
			}
		}
	}
#if 0
	/* Get rid of duplicate strings */
	set<string> s( tasks.begin(), tasks.end() );
	//tasks.erase( unique( tasks.begin(), tasks.end() ), tasks.end() );
	tasks.assign(s.begin(), s.end());
#endif 


	/* Check for voilations */
	for (auto task1 : tasks) {
			for (auto task2: tasks) {
					if (task1.func == task2.func)
							continue;
					for (auto val1: task1.objects) {
							//No need to compare hardcoded values
							//val1->dump();
							if (auto cnst =dyn_cast<ConstantInt>(val1)) {
									continue;
							}
							if (auto func = dyn_cast<Function>(val1)) {
									/* TODO: Should we allow functions to call anything??? */
									continue;
							}
							for (auto val2: task2.objects) {
									if (val1 == val2) {
											cout<< "Tasks are sharing resources:"<<endl;
											dumper(val1);
#if 01
											if (qtype) {
											for(auto user: val1->users()) {
													Value * q;
													if (!qmap.count(val1)) {
														string objName = val1->getName().data();
														string qname = "~" + objName + "q";
														qmap[val1] = createGlobal(ll_mod,qtype,qname);
														
													}
													q =qmap[val1];
													if (auto instruction= dyn_cast<Instruction>(user)) {
														auto BB = instruction->getParent();
														IRBuilder<> Builder(instruction->getParent());
														Value* V = ConstantInt::get(Type::getInt8Ty(BB->getContext()), 0);
													    BinaryOperator::Create(Instruction::Add, V, V, "nop", instruction);
														if (auto str = dyn_cast<llvm::StoreInst>(instruction)) {	
																/* This is a write to the global val */
																if (str->getPointerOperand () == val1) {
																		auto destination = str->getOperand(1); 

																		auto f= ll_mod->getFunction (SEND_FUNC);
																		/*  BaseType_t xQueueSend(
                            QueueHandle_t xQueue,
                            const void * pvItemToQueue,
                            TickType_t xTicksToWait
                         );*/
																		auto ticks = llvm::APInt(32, 10, false);
																		Builder.SetInsertPoint(instruction->getPrevNode());
//																		auto item = Builder.CreateAlloca(Type::getInt8Ty(val1->getContext()),0, "temp");
																		auto OI = instruction->op_begin(); 
																		auto item = Builder.CreateAlloca(OI->get()->getType(),0,"temp");
																		OI++;
																		*OI = item;

																		Builder.SetInsertPoint(instruction->getNextNode());
																		auto item_arg= Builder.CreateBitCast(item, PointerType::get(Type::getInt8Ty(val1->getContext()),0), "item_arg"); 
#if 0
																		for (OI = insn->op_begin(), OE = insn->op_end(); OI != OE; ++OI) {
																			*destination = *item;
																		}
#endif 

																		//item->dump();
																		dumper(instruction->getParent());;
																		//PointerType::get(ticks.getType(),0);

#if 01
																		for (auto& arg : f->args()) {
																		//		arg.getType()->dump();
																		}
																		//q->getType()->dump();
																		//item->getType()->dump();
																		Builder.CreateCall(f,{q,
																						item_arg, 
																						ConstantInt::get(f->getContext(), 
																						llvm::APInt(64, 10, false))});
#endif 

																}
														}
													}
											}
											}
											else {
													cerr << "Q type not found" <<endl;
											}
#endif 
									}
							}
					}
			}
	}

	/* Conduct task-task isolation */
	return error_value;
}
Value * createGlobalPointer(Module * mod, Type * ty, string name) {
		auto init = ConstantPointerNull::get(PointerType::get(ty, 0));
		auto *GV = new llvm::GlobalVariable(
                          *mod, PointerType::get(ty,0), false, GlobalValue::InternalLinkage, init, name);
		return GV;
}
Value * createGlobal(Module * mod, Type * ty, string name) {
        //auto init = ConstantPointerNull::get(ty);
        auto *GV = new llvm::GlobalVariable(
                          *mod, ty, false, GlobalValue::InternalLinkage, nullptr, "");
		GV->setDSOLocal(true);
		//GV->setSection(".init"); //TODO:The section should be called something else, otherwise csu will call this symbol!
		GV->setAlignment(llvm::Align(4));
		// Constant Definitions
		ConstantAggregateZero* const_array_2 = ConstantAggregateZero::get(ty);

		// Global Variable Definitions
		GV->setInitializer(const_array_2);

        return GV;
}
int testPass() {

		for (SVFModule::llvm_iterator F = svfModule->llvmFunBegin(), E = svfModule->llvmFunEnd(); F != E; ++F)
		{
#if 0
				auto task = *F;
				auto name = (*F)->getName().str();
				if (name == "prvQueueReceiveTask") {
						cerr<<"Task found" <<endl;
						for (auto bb=task->begin();bb!=task->end();bb++) {
							 for (auto stmt =bb->begin();stmt!=bb->end(); stmt++) {
                                                cerr<<stmt->getOpcodeName()<<endl;
									 			stmt->dump();
												//continue;
												if (auto call= dyn_cast<CallInst>(stmt)) {
														cout << call->getCalledFunction()->getName().str()<<endl;
														if (call->getCalledFunction()->getName().str() == "_write") {
														//	llvm::ValueToValueMapTy vmap;
															auto *new_inst = call->clone();
															new_inst->insertAfter(stmt->getPrevNode());
														}
												}
                             }	
						}
				}
#endif 
				cerr << (*F)->getName().str() << endl;
		}
		return 1;
}
void updateBC() {
		verifyModule(*ll_mod);
		StringRef file = StringRef("temp.bc");
        std::error_code EC;
            //raw_fd_ostream output = raw_fd_ostream("temp.bc", EC); error: use of deleted function ‘llvm::raw_fd_ostream::raw_fd_ostream(const llvm::raw_fd_ostream&)’
        raw_fd_ostream output("temp.bc", EC);
        llvm::WriteBitcodeToFile(*ll_mod, output);
}
typedef struct {
		string dir;
		string file;
} di;
map<Function*,di> functionDI;
map<unsigned int, vector<Function*>, std::greater<unsigned int>> iomap;
typedef struct {
		string	name;
		string	file;
		string	dir;
		unsigned int addr;
		unsigned int refcount;
} refinfo;
//refcount[address][file] = access count;
map<unsigned int,map<string, int>> refcount;
/* Filtered version of svdmap */
map<int, string> device;
typedef struct {
		APInt Lower;
		APInt Upper;
} constRange;
/* We use this dummy struct because LLVM Constant Range doesn't have a null arg constructor */
/* Device-Driver map */
map<int, refinfo> drivers;
ConstantRange getValueRange(Value * val, Function * fun, Instruction * point);
ConstantRange getValueRange(Value * val, Function * fun, Instruction * point, map<Value *, constRange> known_ranges) {
		if (known_ranges.count(val))
				return ConstantRange(known_ranges[val].Lower.zextOrSelf(32), known_ranges[val].Upper.zextOrSelf(32));
		else 
				return getValueRange(val,fun,point);
} 
//svdmap[base].name;
ConstantRange getValueRange(Value * val, Function * fun, Instruction * point) {
		ConstantRange range = ConstantRange(APInt(32,(unsigned)0,true), 
						APInt(32,(unsigned)0xFFFFFFFF, true));

		if (auto cint= dyn_cast<llvm::ConstantInt>(val)) {
			range = ConstantRange(cint->getValue()); 
			return range;
		}
		/* Collect conditional constraints */
		auto curr = point->getParent();
		vector<list<BasicBlock *>> paths; 
		map<BasicBlock *, int> seen;
		std::queue<BasicBlock *> work;
		work.push(curr);
		list<BasicBlock *> temp;
		temp.push_back(curr);
		paths.push_back(temp);
		val->dump();
		//cerr<<"I am going in!"<<endl;
		//while (curr != &(*fun->begin())) {
		while(!work.empty()) {
				if (seen.count(curr)) {	
						curr = work.front(); work.pop();
						continue;
				}
				else {
						seen[curr] = 1;
				}
				int preds=0;
				while(!curr->hasNPredecessors(preds++));
				preds--; // Adjust for the final one

				/* 
				if (curr->hasNPredecessors(1)) {
						for (path: paths) {
							if (path.back() == curr) {
									path.push_back(BasicBlock);
							}
						}
				}
				else {
				*/
					for (BasicBlock *Pred : predecessors(curr)) {
							int i =0;
							for (auto& path : paths) {
								i++;
								/* Make a copy */
								if (path.back() == curr) {
										work.push(Pred);
										/* No need to fork a list is this is the last pred */
										if (i == preds) {
											path.push_back(Pred);
										}
										else {
											/* Fork the list */
											list<BasicBlock *> temp = path;
											temp.push_back(Pred);
											paths.push_back(temp);
										}
								}
							}
					}
				//}
		}
		for (auto path: paths) {
				cout << "Collecting Constraints" <<endl;
				map<Value * , constRange> known_ranges;
				auto bb = path.cend();
                bb--;
				//(*bb)->dump();
    	            while (bb != path.cbegin()) {
							cout << "Entered Loop" <<endl;
							bool condition;
							auto stmt =(*bb)->end();
							stmt--;
							auto next = bb;
							if (bb != path.cbegin())
									next--;
							if (auto branch = dyn_cast<llvm::BranchInst>(stmt)) {
								if (branch->isConditional()) {
										auto cond = branch->getCondition();
										if (*next == branch->getSuccessor(0)) 
												condition = true;
										else 
												condition = false;

										int trueBranch = 0;int falseBranch =1;

											if (auto cmp = dyn_cast<llvm::CmpInst> (cond)) {
													cmp->dump();
													switch(cmp->getPredicate()) {
															case  CmpInst::FCMP_OEQ:
															case  CmpInst::FCMP_UEQ:
															case  CmpInst::ICMP_EQ:
																	if (condition) {
																			if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(0))) {
																					constRange range;
																					range.Lower = range.Upper = cint->getValue();
																					known_ranges[cmp->getOperand(1)] = range;
																			}
																			else if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) {
																					constRange range;
																					range.Lower = range.Upper = cint->getValue();
																					known_ranges[cmp->getOperand(0)] = range;
																			}

																	}
																	break;
															case CmpInst::ICMP_SGT:
																	if (condition) {
																			 if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(0))) {
																					 constRange range; 
																					 range.Upper = cint->getValue();
																					 range.Lower = APInt (32, 0);
																					 known_ranges[cmp->getOperand(1)] = range;
																			 } else if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) {
																					 constRange range;
                                                                                     range.Lower = cint->getValue();
																					 range.Upper = APInt::getMaxValue(32); 
                                                                                     known_ranges[cmp->getOperand(0)] = range;
																			 }
																	} else {
																			if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(0))) {
																					constRange range;
                                                                                    range.Lower = cint->getValue();
                                                                                    range.Upper = APInt::getMaxValue(32);
                                                                                    known_ranges[cmp->getOperand(1)] = range;
                                                                            } else if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) { 
																					constRange range;
                                                                                     range.Upper = cint->getValue();
                                                                                     range.Lower = APInt (32, 0);
                                                                                     known_ranges[cmp->getOperand(0)] = range;
																			}
																	}
																	break;
															case CmpInst::ICMP_ULT:
															case CmpInst::ICMP_SLT:
																	cout << "Less than"<<endl;
																	if (condition) {
																			if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(0))) {
                                                                                     constRange range;
                                                                                     range.Lower = cint->getValue();
                                                                                     range.Upper = APInt::getMaxValue(32);
                                                                                     known_ranges[cmp->getOperand(1)] = range;
                                                                             } else if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) {
                                                                                     constRange range;
                                                                                     range.Lower = APInt (32, 0);
                                                                                     range.Upper = cint->getValue();
                                                                                     known_ranges[cmp->getOperand(0)] = range;
                                                                             }
																	}
																	else {
																			if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(0))) {
																					 constRange range;
                                                                                     range.Lower = APInt (32, 0);
                                                                                     range.Upper = cint->getValue();
                                                                                     known_ranges[cmp->getOperand(0)] = range;
                                                                             } else if (auto cint= dyn_cast<llvm::ConstantInt>(cmp->getOperand(1))) {
																					 constRange range;
                                                                                     range.Lower = cint->getValue();
                                                                                     range.Upper = APInt::getMaxValue(32);
                                                                                     known_ranges[cmp->getOperand(1)] = range;
                                                                             }
																	}
																	break;
												
															default:
																	cerr<< "Range Analysis needs to extended for constraint collection-cmp"<<endl;
																	break;
																	

													}
											}
								}
							}
							if (bb == path.cbegin()) break;
               		        bb--;
					}
				cout << "******************Propagating constraints" << endl;
				bb = path.cend();
                bb--;
                while (true) {
						for (auto stmt = (*bb)->begin();stmt!=(*bb)->end(); stmt++) {
                                for (auto op : stmt->operand_values()) {
										if (known_ranges.count(op)) {
												if (auto bop= dyn_cast<llvm::BinaryOperator>(stmt)) {
														switch(bop->getOpcode()) {
																case Instruction::LShr:
																		/* TODO: We can leave LSHR for recursive loop */
																case Instruction::And:
																		/* TODO: How to extend and??*/
																		break;
																default:
																		cout<<"Binary Operator needs to be extended for prop analysis"<<endl;
																		stmt->dump();
														}
												}
												else if (auto sext = dyn_cast<llvm::SExtInst>(stmt)) {
														known_ranges[&(*stmt)] = known_ranges[op];
												}
												else if (auto cmp = dyn_cast<llvm::CmpInst> (stmt)){
														//Nothing to extend for comparisons 
												}
												else {
														cout<<"Need to extend the propogation analysis"<<endl;
														stmt->dump();
												}
										}
								}
						}
						if (bb == path.cbegin()) break;
                            bb--;
				}
				cout <<"*******************Running Dumbass Loop" <<endl;
				bb = path.cend();
				bb--;
				while (true) {

						
						for (auto stmt = (*bb)->begin();stmt!=(*bb)->end(); stmt++) {
								for (auto op : stmt->operand_values()) {
										if (op == val) {
												//stmt->dump();
												if (auto bop= dyn_cast<llvm::BinaryOperator>(op)) {
														switch(bop->getOpcode()) {
																case Instruction::LShr:
																		{
																		//bop->dump();
																		for (auto op1 : bop->operand_values()) {
																				op1->dump();
																		}
																		auto val_range = getValueRange(bop->getOperand(0), fun, bop, known_ranges);
																		auto shift_range = getValueRange(bop->getOperand(1), fun, bop, known_ranges);
																		auto valL = val_range.getLower();
																		auto valH = val_range.getUpper();
																		/* [Min<<Min , Max<<Max] */
																		auto retL = valL.lshr(shift_range.getLower());
																		auto retH = valH.lshr(shift_range.getUpper());
																		return ConstantRange(retL, retH);
																		break;
																		}
																case Instruction::AShr: 
																		{
																				auto val_range = getValueRange(bop->getOperand(0), fun, bop, known_ranges);
		                                                                        auto shift_range = getValueRange(bop->getOperand(1), fun, bop, known_ranges);
        		                                                                auto valL = val_range.getLower();
                		                                                        auto valH = val_range.getUpper();
																				/* Out of the product we pick: [Min >> Max, Max >> Min]*/
                        		                                                auto retL = valL.ashr(shift_range.getUpper());
                                		                                        auto retH = valH.ashr(shift_range.getLower());
                                        		                                return ConstantRange(retL, retH);
																				break;
																		}
																case Instruction::Shl:
																		{
                                                                                auto val_range = getValueRange(bop->getOperand(0), fun, bop, known_ranges);
                                                                                auto shift_range = getValueRange(bop->getOperand(1), fun, bop, known_ranges);
                                                                                auto valL = val_range.getLower();
                                                                                auto valH = val_range.getUpper();
                                                                                /* Out of the product we pick: [Min << Min, Max << Max]*/
                                                                                auto retL = valL.shl(shift_range.getLower());
                                                                                auto retH = valH.ashr(shift_range.getUpper());
                                                                                return ConstantRange(retL, retH);
                                                                                break;
                                                                        }

																default: 
																		cerr<< "Range Analysis needs to be extended for Binary Operators" <<endl;
																		bop->dump();
																		break;
														}
												}
												else if (auto arg= dyn_cast<llvm::Argument>(op)) {
														/* TODO: For Arguments go through all callsites to find possible calls */
														cerr << "Argument Found, Returning Max Range" <<endl;
														cerr << "*****************" <<endl;
														arg->dump();
														auto argnum = 	arg->getArgNo();
														vector<int> callee_vals;
														int num_callees = 0;
														for (auto caller : fun->users()) {
																num_callees++;
																caller->dump();
																if (auto call = dyn_cast<llvm::CallInst>(caller)) {
																		auto concrete_arg = call->getArgOperand (argnum);
																		concrete_arg->dump();
																		if (auto cint= dyn_cast<llvm::ConstantInt>(concrete_arg)) {
																            callee_vals.push_back((unsigned)*cint->getValue().getRawData());
																        }
																		else {
																				return ConstantRange(APInt(32,(unsigned)0,true),
				                                                                    APInt(32,(unsigned)0xFFFFFFFF, true));
																		}
																}
														}
														if(num_callees == 0) {
																return ConstantRange(APInt(32,(unsigned)0,true),APInt(32,(unsigned)0,true));
														}
														else {
																/* We got here because all of the callers used constant values,
																 find the min and max and return to the caller */
																unsigned min = *min_element(callee_vals.begin(), callee_vals.end());
																unsigned max = *max_element(callee_vals.begin(), callee_vals.end());
																cout << "["<<min<<","<<max<<"]"<<endl;
																return ConstantRange(APInt(32,(unsigned)*min_element(callee_vals.begin(), callee_vals.end()),true),
																				APInt(32,(unsigned)*max_element(callee_vals.begin(), callee_vals.end()),true));

														}
														return ConstantRange(APInt(32,(unsigned)0,true),
											                        APInt(32,(unsigned)0xFFFFFFFF, true));
												}
												else if (auto sext = dyn_cast<llvm::SExtInst>(op)) {
														sext->dump();
														return getValueRange(sext->getOperand(0), fun, sext, known_ranges);
												}

												else {
														cerr<< "Range Analysis needs to be extended for general Instruction" <<endl;
														op->dump();
														
												}
										}
								}
						}
						if (bb == path.cbegin()) break;
						bb--;

    			}
        }

		/* Collect arithmetic constraints */

		return range;
}
int driverIsolation() {
	for (SVFModule::llvm_iterator F = svfModule->llvmFunBegin(), E = svfModule->llvmFunEnd(); F != E; ++F)
	{
		auto fun = *F;
		//if (fun->getName().str() == "__NVIC_SetPriority.201") {
		if (true){
		for (auto bb=fun->begin();bb!=fun->end();bb++) {
				for (auto stmt =bb->begin();stmt!=bb->end(); stmt++) {
						/* Iterate all operands and see if there is inttoptr */
						for (auto op : stmt->operand_values()) {
								if (auto cast= dyn_cast<llvm::Instruction>(op)) {
//										cout<< "********************************************"<<endl;
								}
								/* Harcoded pointers might appear as Constant Expressions */
								if (auto cast= dyn_cast<llvm::ConstantExpr>(op)) {
										/* Get the thing as an instruction */
										if (auto inttoptr = dyn_cast<llvm::IntToPtrInst>(cast->getAsInstruction())) {
												cout << fun->getName().str() << " accesses "; inttoptr->dump();
												if (auto ptsTo = dyn_cast<llvm::ConstantInt>(inttoptr->getOperand(0))) {
													auto addr = *ptsTo->getValue().getRawData();
													if (addr == 0 || addr ==0xFFFFFFFF) {
															/* These values could be error codes or a weird 
															 way to make a nullptr */
															continue;
													}
													/* OK, this is a pointer, let's sanitize the shit out of its usage */
													if (isa<llvm::StoreInst>(stmt) || isa<llvm::LoadInst>(stmt)) {
													   if (stmt->hasNUsesOrMore(2)) {
													   cout<<"*****************************"<<endl;
                                         		       stmt->dump();
													   printDI(dyn_cast<llvm::Instruction>(stmt));
													   cout<<"used by:  "<<endl;
													   int i =0;
													  	 for (auto user: stmt->users()) {
																   user->dump();
																   printDI(dyn_cast<llvm::Instruction>(user));
													     }
													   }
                                        			}
													if (auto gep = dyn_cast<llvm::GetElementPtrInst>(stmt)) {
															/* Trying to escape with pointer arithmetic not allowed */
															gep->dump();
															if (!gep->hasAllConstantIndices()) {
																gep->getOperand(1)->dump();
																gep->getOperand(2)->dump();
																auto cr = computeConstantRange(gep->getOperand(2));
#if 0
																cerr<<"Lower :"<<*cr.getLower().getRawData() << endl;
																cerr<<"Upper :"<<*cr.getUpper().getRawData() << endl;
																cerr<<"Cannot resolve pointer"<<endl;
#endif 
	
																cr = getValueRange(gep->getOperand(2), fun, &(*stmt));
#if 0
																cerr<<"Lower :"<<*cr.getLower().getRawData() << endl;
                                                                cerr<<"Upper :"<<*cr.getUpper().getRawData() << endl;
																printDI(dyn_cast<llvm::Instruction>(stmt));
#endif 
																cerr<<"Upper :"<<*cr.getUpper().getRawData() << endl;
																if (*cr.getUpper().getRawData() > 4096) {
																	cerr<<"Cannot resolve pointer"<<endl;
																	cerr<<"Lower :"<<*cr.getLower().getRawData() << endl;
																	cerr<<"Upper :"<<*cr.getUpper().getRawData() << endl;
																	cerr<<"Function:"<<fun->getName().str()<<endl;
																	printDI(dyn_cast<llvm::Instruction>(stmt));
																	exit(1);
																}
															}
													}
													cout << std::hex <<"0x"<<*ptsTo->getValue().getRawData() <<endl;
													if (!vContains(iomap[addr], fun)) {
																	iomap[(unsigned int)addr].push_back(fun);
													}
													auto instruction = stmt;
													const llvm::DebugLoc &debugInfo = instruction->getDebugLoc();
	                        						if (debugInfo.getAsMDNode())    {
    	                    							StringRef directory = debugInfo->getDirectory();
        	                							StringRef filePath = debugInfo->getFilename();
														di temp =  {.dir=directory.str(),.file= filePath.str()};
														functionDI[fun] = temp;
            	            							int line = debugInfo->getLine();
                	        							int column = debugInfo->getColumn();
                    	    							cerr<<"Error: "<<directory.str()  << "/"<<filePath.str() << ":" << line << ":" << column<<endl;
														addr &= ~(0x1000 -1);
														refcount[addr][directory.str() + filePath.str()] +=1;
                        							}
                        							else {
                                						cerr<<"Fauly instruction with no DI:";
                        							}
												}
										}
#if 0
										if (cast->isCast()) {
											cout <<"++++++++++++++++++++++++++++++++++++++++++++"<<endl;
											cout << cast->getOpcodeName()<<endl;
										}

										for (auto op2: cast->operand_values()) {
												cout<<"Hello"<<endl;
												op2->dump();
										}
#endif 
								}
						}
						/* Load */

						/* Store */
						/* GEP */
				}
		}
		}
	}
#if 0
	vector <int> temp;
	for (const std::pair<int, vector<Function*>> &p:iomap) {
			cout <<p.first <<endl;
			temp.push_back(p.first);
			for (auto fun: p.second) {
					cout<<functionDI[fun].dir << functionDI[fun].file <<endl;
			}
	}

	std::sort(temp.begin(), temp.end());
	for (int iter: temp) {
			int base =iter & ~(0x1000 -1);
			if (svdmap.count(base)) {
				device[base] = svdmap[base].name;
			} else {
				device[base] = "Unknown Device/Probably Wild Pointer Error";
			}
	}

	for (const std::pair<int, string> &p:device) {
            cout <<p.second << ":" <<p.first <<endl;
    }
#endif 
	for (const std::pair<unsigned int,map<string, int>> &p : refcount) {
			int err;
			if (p.second.size() > 1)  
				cout<< "Error: " << svdmap[p.first].name <<" accesed by multiple modules"<<endl;
					refinfo owner;
					owner.refcount = 0;
					owner.addr = p.first;
					if (!svdmap[p.first].name.empty()) {
                    	owner.name = svdmap[p.first].name;
                    } else {
                        owner.name = "Unknown Device/Probably Wild Pointer Error";
                    }
					for (auto &fileRef: p.second) {
						if (p.second.size() > 1 && !svdmap[p.first].driver.empty())
                    		cout <<"    "<<fileRef.first<<" "<<fileRef.second << " times"<<endl;
							/* See if we were given a driver that takes precedence */
							if (!svdmap[p.first].driver.empty()) {
									if (fileRef.first == svdmap[p.first].driver) {
										owner.file = svdmap[p.first].driver;
										owner.refcount = fileRef.second;
									}
							}
							else 
							/* TODO: make the sole accessor the file that acceses the function most */
							if (owner.refcount < fileRef.second) {
			                    owner.file = fileRef.first;
                    			owner.refcount = fileRef.second;
							}
            		}
					drivers[p.first] = owner;
#if 0
			if (svdmap.count(p.first)) {
                cout<< svdmap[p.first].name;
            } else {
                cout<< "Unknown Device/Probably Wild Pointer Error";
            }
#endif 
	}

            cout<<"Drivers Found:" <<endl;
            for(auto p:drivers) {
                    cout<<p.second.name << p.second.addr<< " owned by" <<p.second.file<<endl;
            }
	return 0;
}
int main(int argc, char ** argv) {
	int error_value = 0;
	parseArguments(argc,argv);
	buildPTA();


	//error_value = testPass();
	/* First check task-kernel voilations */
	error_value = driverIsolation();
#if 0
	error_value = taskKernelVoilations();
	if (!error_value) {
			/* Check task-task voilations */
			error_value = taskTaskVoilations();
	}
#endif 

	int i =0;
	//We can create our own queue type but let's use program provided queue
	for (auto is: ll_mod->getIdentifiedStructTypes()) {
#if 0
			//ll_mod->getOrInsertGlobal("temp" + to_string(i), is);
			auto C = ll_mod->getOrInsertGlobal(gen_random(12), is);
			if (!C){
				cout<<"The heck!"<<endl;
			}
			else {
					cout<<"What do we have here?"<< endl;
					C->dump();
			}
			if (GlobalVariable *G = dyn_cast_or_null<GlobalVariable>(C)) {
		      G->setVisibility(GlobalValue::DefaultVisibility);
			  G->setLinkage(GlobalValue::InternalLinkage );
			  G->dump();
			}
#endif
			string s2 = "queue";
			string type = is->getName().data();
			/* Find queue type */
			if (type.find(s2) != std::string::npos) {
			   qtype = is;
			}
	}
	/* First check task-kernel voilations */
    //error_value = taskKernelVoilations();
    if (!error_value) {
            /* Check task-task voilations */
            //error_value = taskTaskVoilations();
    }
	verifyModule(*ll_mod);

	//TODO: This should be changed to an option that updates the code instead
	if (true) {
			StringRef file = StringRef("temp.bc");
			std::error_code EC;
			//raw_fd_ostream output = raw_fd_ostream("temp.bc", EC); error: use of deleted function ‘llvm::raw_fd_ostream::raw_fd_ostream(const llvm::raw_fd_ostream&)’
			raw_fd_ostream output("temp.bc", EC);
			llvm::WriteBitcodeToFile(*ll_mod, output);
	}

	//Temp for debugging and hacking 
	return error_value;
}

