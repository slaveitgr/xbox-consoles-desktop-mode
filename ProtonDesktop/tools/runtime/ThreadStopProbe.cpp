// SPDX-License-Identifier: GPL-2.0-only
// Real pinned kernel cancellation regression. No Wine executable or ELF runs.
// A test-only scheduler hook orders an actual termination before wait entry.
// Baseline counterexamples are released externally, then genuinely joined;
// fixed cases must complete within 200 ms before any rescue. No detached host
// threads and no successful return while a worker can still access the kernel.
#include "boxedwine.h"
#include "kmemory64.h"
#include "knativesystem.h"
#include "kunixsocket.h"
#include "ksocket.h"
#include "kscheduler.h"
#include "backend/uwp/UwpGuestWorkers.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <functional>
using namespace protondesktop::uwp;
struct FixtureSocket : KUnixSocketObject {
 FixtureSocket():KUnixSocketObject(K_AF_UNIX,K_SOCK_STREAM,0){}
 std::shared_ptr<BoxedWineCondition> Condition(){return lockCond;}
};
std::atomic<U32> marked{0};
void protonFixtureTerminationMarked(U32 tid) { marked.store(tid, std::memory_order_release); }
bool until(const std::function<bool()>& condition, unsigned ms=2000) {
 auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
 while(!condition()){if(std::chrono::steady_clock::now()>end)return false;std::this_thread::yield();}return true;
}
int main(int argc,char**argv) {
 if(argc!=2)return 64;
 setvbuf(stdout,nullptr,_IONBF,0);
 std::printf("LAYOUT KThread=%zu terminating=%zu waitingCond=%zu waitingCondSync=%zu KProcess=%zu\n",sizeof(KThread),offsetof(KThread,terminating),offsetof(KThread,waitingCond),offsetof(KThread,waitingCondSync),sizeof(KProcess));
 Platform::init();if(!KNativeSystem::init(VIDEO_NO_WINDOW,false))return 65;
 KSystem::init();if(!Fs::initFileSystem(BString::copy(argv[1])))return 66;
 Fs::makeLocalDirs(B("/proc"));KSystem::procNode=Fs::getNodeFromLocalPath(B(""),B("/proc"),true);
 std::string error; if(!BeginGuestWorkerSession(error))return 67;
 unsigned counterexamples=0;
 for(const char* name:{"socket-before-registration","futex-before-registration"}) {
  bool futex=name[0]=='f';auto process=KProcess::create();process->memory=KMemory::create(process.get());auto thread=process->createThread();U32 tid=thread->id;
  U64 address=0;
  if(futex){process->is64Bit=true;process->memory64=new KMemory64(process.get());address=process->memory64->mmapReserveAndMap(4096,3);process->memory64->writed(address,0);}
  auto socket=std::make_shared<KUnixSocketObject>(K_AF_UNIX,K_SOCK_STREAM,0);
  auto peer=std::make_shared<KUnixSocketObject>(K_AF_UNIX,K_SOCK_STREAM,0);
  socket->connection=peer;peer->connection=socket;socket->setBlocking(true);
  std::promise<void> ready,enter;auto entered=enter.get_future().share();auto started=ready.get_future();
  std::promise<void> siblingRelease,siblingReady;auto siblingAllowed=siblingRelease.get_future().share();auto siblingStarted=siblingReady.get_future();
  if(futex){auto sibling=process->createThread();if(!SpawnGuestWorker([&,process,sibling]{KThread::setCurrentThread(sibling);siblingReady.set_value();siblingAllowed.wait();process->deleteThread(sibling);KThread::setCurrentThread(nullptr);},process->id,process,error))return 74;siblingStarted.wait();}
  std::atomic<S64> syscallResult{12345};
  if(!SpawnGuestWorker([&,process,thread,socket]{
   KThread::setCurrentThread(thread);ready.set_value();entered.wait();
   U8 byte=0;syscallResult=futex?thread->futex64(address,0,0,0,0xffffffff):(S32)socket->readNative(&byte,1);
   process->deleteThread(thread);KThread::setCurrentThread(nullptr);
  },process->id,process,error))return 68;
  started.wait();marked=0;std::atomic<bool> controllerDone{false};
  std::thread controller([&]{KThread::setCurrentThread(nullptr);if(futex)terminateOtherThread(process,tid);else process->killAllThreads();controllerDone=true;});
  if(!until([&]{return marked.load(std::memory_order_acquire)==tid;}))std::_Exit(69);
  // The actual controller marked this KThread while it was outside a kernel
  // wait. Only now let it enter the actual socket/futex implementation.
  enter.set_value();
  bool beforeRescue=until([&]{return controllerDone.load();},200);
  if(!beforeRescue){++counterexamples;if(futex)thread->futex64(address,1,1,0,0xffffffff);else socket->signalReadReady();}
  if(!until([&]{return controllerDone.load();}))std::_Exit(70);
  controller.join();if(futex)siblingRelease.set_value();
  if(!until([&]{ReapCompletedGuestWorkers(error);return !ProcessHasActiveGuestWorkers(process->id);} ))std::_Exit(71);
  std::printf("CASE %s completed_before_external_rescue=%d actual_syscall_result=%lld remaining_threads=%u\n",name,beforeRescue,(long long)syscallResult.load(),process->getThreadCount());
  if(syscallResult!=-K_EINTR||process->getThreadCount()!=0)return 72;
  KSystem::eraseProcess(process->id);process.reset();socket.reset();peer.reset();
 }
 {
  auto process=KProcess::create();process->memory=KMemory::create(process.get());
  auto first=process->createThread();auto target=process->createThread();const U32 tid=target->id;
  auto socket=std::make_shared<FixtureSocket>();auto peer=std::make_shared<FixtureSocket>();
  socket->connection=peer;peer->connection=socket;socket->setBlocking(true);
  auto condition=socket->Condition();std::atomic<S64> firstResult{12345},targetResult{12345};
  auto spawnReader=[&](KThread* thread,std::atomic<S64>& result){
   return SpawnGuestWorker([&,thread,process,socket]{KThread::setCurrentThread(thread);U8 byte=0;result=(S32)socket->readNative(&byte,1);process->deleteThread(thread);KThread::setCurrentThread(nullptr);},process->id,process,error);
  };
  auto parked=[&](KThread* thread){std::unique_lock<std::mutex> lock(condition->m);return thread->waitingCond==condition;};
  if(!spawnReader(first,firstResult)||!until([&]{return parked(first);}))std::_Exit(75);
  if(!spawnReader(target,targetResult)||!until([&]{return parked(target);}))std::_Exit(76);
  const auto joinedBefore=GetGuestWorkerSnapshot().joined;std::atomic<bool> done{false};
  std::thread controller([&]{KThread::setCurrentThread(nullptr);terminateOtherThread(process,tid);done=true;});
  const bool beforeRescue=until([&]{return done.load();},200);
  if(!beforeRescue){++counterexamples;socket->signalReadReady();}
  if(!until([&]{return done.load();}))std::_Exit(77);controller.join();
  if(!until([&]{ReapCompletedGuestWorkers(error);return GetGuestWorkerSnapshot().joined==joinedBefore+1;}))std::_Exit(78);
  U8 byte=42;if(peer->writeNative(&byte,1)!=1)std::_Exit(79);
  if(!until([&]{ReapCompletedGuestWorkers(error);return !ProcessHasActiveGuestWorkers(process->id);} ))std::_Exit(80);
  std::printf("CASE shared-socket-targeted-kill completed_before_external_rescue=%d target_result=%lld surviving_reader_result=%lld remaining_threads=%u\n",beforeRescue,(long long)targetResult.load(),(long long)firstResult.load(),process->getThreadCount());
  if(targetResult!=-K_EINTR||firstResult!=1||process->getThreadCount()!=0)return 81;
  KSystem::eraseProcess(process->id);process.reset();socket.reset();peer.reset();
 }
 {
  auto process=KProcess::create();process->memory=KMemory::create(process.get());
  auto blocked=process->createThread();auto exiting=process->createThread();const U32 blockedTid=blocked->id;
  auto socket=std::make_shared<FixtureSocket>();auto peer=std::make_shared<FixtureSocket>();
  socket->connection=peer;peer->connection=socket;socket->setBlocking(true);
  std::promise<void> blockedReady,blockedRelease,exitingReady,exitRelease;
  auto blockedStarted=blockedReady.get_future();auto mayBlock=blockedRelease.get_future().share();
  auto exitingStarted=exitingReady.get_future();auto mayExit=exitRelease.get_future().share();
  std::atomic<S64> blockedResult{12345};std::atomic<U32> exitReturn{0};
  if(!SpawnGuestWorker([&,process,blocked,socket]{KThread::setCurrentThread(blocked);blockedReady.set_value();mayBlock.wait();U8 byte=0;blockedResult=(S32)socket->readNative(&byte,1);process->deleteThread(blocked);KThread::setCurrentThread(nullptr);},process->id,process,error))return 82;
  if(!SpawnGuestWorker([&,process,exiting]{KThread::setCurrentThread(exiting);exitingReady.set_value();mayExit.wait();exitReturn=process->exitgroup(exiting,0);process->deleteThread(exiting);KThread::setCurrentThread(nullptr);},process->id,process,error))return 83;
  blockedStarted.wait();exitingStarted.wait();marked=0;exitRelease.set_value();
  if(!until([&]{return marked.load(std::memory_order_acquire)==blockedTid;}))std::_Exit(84);
  marked=0;std::atomic<bool> controllerDone{false};
  std::thread controller([&]{KThread::setCurrentThread(nullptr);process->killAllThreads();controllerDone=true;});
  if(!until([&]{return marked.load(std::memory_order_acquire)!=0;}))std::_Exit(85);
  blockedRelease.set_value();
  const bool beforeRescue=until([&]{return controllerDone.load();},200);
  if(!beforeRescue){++counterexamples;socket->signalReadReady();}
  if(!until([&]{return controllerDone.load();}))std::_Exit(86);controller.join();
  if(!until([&]{ReapCompletedGuestWorkers(error);return !ProcessHasActiveGuestWorkers(process->id);} ))std::_Exit(87);
  std::printf("CASE guest-exitgroup-plus-host-controller completed_before_external_rescue=%d blocked_result=%lld exit_return=%u process_terminated=%d exit_code=%u remaining_threads=%u\n",beforeRescue,(long long)blockedResult.load(),exitReturn.load(),process->isTerminated(),process->exitCode,process->getThreadCount());
  if(blockedResult!=-K_EINTR||!process->isTerminated()||process->exitCode!=0||process->getThreadCount()!=0)return 88;
  KSystem::eraseProcess(process->id);process.reset();socket.reset();peer.reset();
 }
 CloseGuestWorkerAdmission();if(!JoinGuestWorkers(error)||!EndGuestWorkerSession(error))return 73;
 KSystem::destroy();KNativeSystem::shutdown();KNativeSystem::cleanup();
 std::printf("ALL_REAL_WORKERS_JOINED counterexamples=%u Wine_executed=0\n",counterexamples);
 return counterexamples?23:0;
}
