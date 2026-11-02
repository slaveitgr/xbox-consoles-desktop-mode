// SPDX-License-Identifier: GPL-2.0-only
// Real X11 wire records and SDL input, no Wine/rootfs or fabricated UI.
#include "boxedwine.h"
#include "xwireserver.h"
#include "xwirepresent.h"
#include "kunixsocket.h"
#include "ksocket.h"
#include "knativesystem.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <stdexcept>
void protonConfigureXWireDesktop(unsigned,unsigned);
void protonWineGuestFrameSubmitted(unsigned,unsigned){}
static int failures=0;
void check(bool ok,const char*text){std::printf("%s: %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;}
void p16(std::vector<U8>&v,unsigned p,U16 x){std::memcpy(v.data()+p,&x,2);}
void p32(std::vector<U8>&v,unsigned p,U32 x){std::memcpy(v.data()+p,&x,4);}
U32 r32(const U8*p){U32 x;std::memcpy(&x,p,4);return x;}
S16 r16(const U8*p){S16 x;std::memcpy(&x,p,2);return x;}
std::vector<U8> readAll(const std::shared_ptr<KUnixSocketObject>&socket){std::vector<U8>out;U8 block[4096];for(;;){U32 n=socket->readNative(block,sizeof(block));if((S32)n<=0)break;out.insert(out.end(),block,block+n);}return out;}
void write(const std::shared_ptr<KUnixSocketObject>&socket,std::vector<U8>v){if(socket->writeNative(v.data(),v.size())!=v.size())throw std::runtime_error("actual socket write failed");}
struct Client{std::shared_ptr<KUnixSocketObject>socket;U32 base=0,window=0;};
Client connectClient(){Client c;c.socket=std::make_shared<KUnixSocketObject>(K_AF_UNIX,K_SOCK_STREAM,0);c.socket->setBlocking(false);if(!XWireServer::instance().acceptConnection(c.socket))throw std::runtime_error("accept failed");std::vector<U8>hello(12,0);hello[0]='l';p16(hello,2,11);write(c.socket,hello);auto response=readAll(c.socket);check(response.size()>=20&&response[0]==1,"real X11 handshake succeeds");c.base=r32(response.data()+12);c.window=c.base|1;return c;}
void makeWindow(Client&c,U32 parent,int x,int y,int w,int h){std::vector<U8>v(36,0);v[0]=1;v[1]=24;p16(v,2,9);p32(v,4,c.window);p32(v,8,parent);p16(v,12,x);p16(v,14,y);p16(v,16,w);p16(v,18,h);p16(v,22,1);p32(v,24,0x21);p32(v,28,1u<<11);p32(v,32,0x4f);write(c.socket,v);v.assign(8,0);v[0]=8;p16(v,2,2);p32(v,4,c.window);write(c.socket,v);readAll(c.socket);}
void pointer(unsigned type,int x,int y){SDL_Event event{};event.type=type;if(type==SDL_MOUSEMOTION){event.motion.x=x;event.motion.y=y;}else{event.button.button=SDL_BUTTON_LEFT;event.button.x=x;event.button.y=y;}xwireForwardSdlEvent(event);}
void key(unsigned type){SDL_Event e{};e.type=type;e.key.keysym.scancode=SDL_SCANCODE_A;xwireForwardSdlEvent(e);}
std::vector<std::array<U8,32>> inputs(const std::vector<U8>&bytes){std::vector<std::array<U8,32>>out;for(size_t i=0;i+32<=bytes.size();i+=32){if(bytes[i]>=2&&bytes[i]<=6){std::array<U8,32>e;std::memcpy(e.data(),bytes.data()+i,32);out.push_back(e);}}return out;}
int main(){Platform::init();if(!KNativeSystem::init(VIDEO_NORMAL,false))return 64;KSystem::init();KSystem::videoOption=VIDEO_NORMAL;protonConfigureXWireDesktop(1280,720);installXWireSink();check(g_xwirePresentSink!=nullptr,"real SDL input sink installed");auto first=connectClient(),second=connectClient();check(first.base&&second.base&&first.base!=second.base,"clients have separate real resource bases");makeWindow(first,0x260,0,0,1280,720);makeWindow(second,first.window,100,200,300,200);{auto&srv=XWireServer::instance();std::lock_guard<std::mutex>lock(srv.regMutex);srv.presentWindow=0x260;}readAll(first.socket);readAll(second.socket);
std::vector<U8> queryFocus(4,0);queryFocus[0]=43;p16(queryFocus,2,1);write(first.socket,queryFocus);auto initialFocus=readAll(first.socket);check(initialFocus.size()==32&&r32(initialFocus.data()+8)==0,"desktop canvas root is never reported as keyboard focus");
pointer(SDL_MOUSEMOTION,110,210);pointer(SDL_MOUSEBUTTONDOWN,110,210);pointer(SDL_MOUSEBUTTONUP,110,210);key(SDL_KEYDOWN);key(SDL_KEYUP);XWireServer::instance().pumpInput();auto a=inputs(readAll(first.socket)),b=inputs(readAll(second.socket));check(a.empty(),"root canvas does not route second-client events into first client");check(b.size()==5,"second client receives each motion/button/key event once");if(b.size()==5){for(const auto&e:b){check(r32(e.data()+12)==second.window,"wire event window belongs to actual recipient client");check(r16(e.data()+20)==110&&r16(e.data()+22)==210,"wire event preserves root coordinates");check(r16(e.data()+24)==10&&r16(e.data()+26)==10,"wire event translates nested local coordinates");}check(b[1][0]==4&&b[2][0]==5&&b[3][0]==2&&b[4][0]==3,"ordered button and key wire events retained");check((r16(b[1].data()+28)&256)==0&&(r16(b[2].data()+28)&256)!=0,"button state is before press and before release");}
check(!g_xwirePresentSink->hasInput(),"central dispatcher consumes host queue exactly once");pointer(SDL_MOUSEBUTTONDOWN,20,20);pointer(SDL_MOUSEBUTTONUP,20,20);key(SDL_KEYDOWN);key(SDL_KEYUP);XWireServer::instance().pumpInput();a=inputs(readAll(first.socket));b=inputs(readAll(second.socket));check(a.size()==4&&b.empty(),"next cross-client batch reaches first client only");if(a.size()==4)for(const auto&e:a)check(r32(e.data()+12)==first.window,"focus follows actual clicked first-client window");
std::vector<U8>focus(12,0);focus[0]=42;p16(focus,2,3);p32(focus,4,second.window);write(second.socket,focus);readAll(second.socket);key(SDL_KEYDOWN);key(SDL_KEYUP);XWireServer::instance().pumpInput();a=inputs(readAll(first.socket));b=inputs(readAll(second.socket));check(a.empty()&&b.size()==2,"keyboard follows guest focus across clients despite pointer elsewhere");if(b.size()==2)check(r16(b[0].data()+24)==-80&&r16(b[0].data()+26)==-180,"keyboard pointer coordinates remain relative to focus window");check(!g_xwirePresentSink->hasInput(),"queue empty after focused cross-client delivery");std::printf("REAL_WIRE_INPUT_RESULT failed=%d\n",failures);g_xwirePresentSink=nullptr;KSystem::destroy();KNativeSystem::shutdown();KNativeSystem::cleanup();return failures?1:0;}
