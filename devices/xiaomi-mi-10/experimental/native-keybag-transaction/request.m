/* Use the original KBXPCProtocol, with an explicit private service name.
 * Never call MKBKeyBagChangeSystemSecret: it addresses the live service. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include "test-secrets.h"
static NSFileHandle *secret_pipe(const char *value) {
 int fd[2];if(pipe(fd))return nil;size_t n=strlen(value);
 if(write(fd[1],value,n)!=(ssize_t)n){close(fd[0]);close(fd[1]);return nil;}
 close(fd[1]);return [[NSFileHandle alloc] initWithFileDescriptor:fd[0] closeOnDealloc:YES];
}
int native_request_pair(int old_index,int new_index) {
 @autoreleasepool {
  if(old_index < -1 || new_index < -1 || (old_index == -1 && new_index == -1) || old_index>=(int)(sizeof(pins)/sizeof(pins[0])) ||
     new_index>=(int)(sizeof(pins)/sizeof(pins[0])))return 2;
  void *lib=dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag",RTLD_NOW);
  Protocol *p=lib?objc_getProtocol("KBXPCProtocol"):nil;if(!p)return 3;
  SEL method=NSSelectorFromString(@"changeSystemSecretfromOldSecret:oldSize:toNewSecret:newSize:opaqueData:reply:");
  struct objc_method_description d=protocol_getMethodDescription(p,method,YES,YES);
  if(!d.name||!d.types)return 4;
  fprintf(stderr,"UMI native txn request: explicit private service, original protocol types=%s\n",d.types);
  id c=((id(*)(id,SEL,id,NSUInteger))objc_msgSend)([NSClassFromString(@"NSXPCConnection") alloc],NSSelectorFromString(@"initWithMachServiceName:options:"),@"com.hakio.mobile.keybagd.xpc",0);
  id interface=((id(*)(id,SEL,Protocol*))objc_msgSend)(NSClassFromString(@"NSXPCInterface"),NSSelectorFromString(@"interfaceWithProtocol:"),p);
  if(!c||!interface)return 5;
  ((void(*)(id,SEL,id))objc_msgSend)(c,NSSelectorFromString(@"setRemoteObjectInterface:"),interface);
  ((void(*)(id,SEL))objc_msgSend)(c,NSSelectorFromString(@"resume"));
  dispatch_semaphore_t sem=dispatch_semaphore_create(0);__block int result=-1;
  void (^error)(NSError*)=^(NSError *err){fprintf(stderr,"UMI native txn request: XPC error domain=%s code=%ld description=%s\n",err.domain.UTF8String,(long)err.code,err.localizedDescription.UTF8String);result=-2;dispatch_semaphore_signal(sem);};
  id proxy=((id(*)(id,SEL,id))objc_msgSend)(c,NSSelectorFromString(@"remoteObjectProxyWithErrorHandler:"),error);
  const char *old=old_index>=0?pins[old_index]:"",*next=new_index>=0?pins[new_index]:"";
  NSFileHandle *a=secret_pipe(old),*b=secret_pipe(next);if(!a||!b)return 7;
  void (^reply)(NSError*)=^(NSError *err){result=err?(int)err.code:0;dispatch_semaphore_signal(sem);};
  ((void(*)(id,SEL,id,uint64_t,id,uint64_t,id,id))objc_msgSend)(proxy,method,a,(uint64_t)strlen(old),b,(uint64_t)strlen(next),nil,reply);
  long timeout=dispatch_semaphore_wait(sem,dispatch_time(DISPATCH_TIME_NOW,60LL*NSEC_PER_SEC));
  ((void(*)(id,SEL))objc_msgSend)(c,NSSelectorFromString(@"invalidate"));
  fprintf(stderr,"UMI native txn request: fixture-pair=%d/%d result=%d timeout=%ld\n",old_index,new_index,result,timeout);
  return timeout||result?6:0;
 }
}
int native_request(int index){return native_request_pair(index-1,index);}
