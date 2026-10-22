#pragma once
#import <Foundation/Foundation.h>
// Fetch only the URL explicitly requested by the project; no implicit CDN or version policy.
inline NSData* nativeFetchResource(NSString* address) {
 NSURL* url=[NSURL URLWithString:address];
 if(!url||![@[@"https",@"http"] containsObject:url.scheme.lowercaseString])return nil;
 NSURLSessionConfiguration* config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
 config.timeoutIntervalForRequest=30;config.timeoutIntervalForResource=60;
 NSURLSession* session=[NSURLSession sessionWithConfiguration:config];
 dispatch_semaphore_t ready=dispatch_semaphore_create(0);__block NSData* result=nil;
 [[session dataTaskWithURL:url completionHandler:^(NSData* data,NSURLResponse* response,NSError* error){
  NSInteger status=[response isKindOfClass:NSHTTPURLResponse.class]?[(NSHTTPURLResponse*)response statusCode]:0;
  if(!error&&status>=200&&status<300)result=data;
  else fprintf(stderr,"[resource-http] %s status=%ld error=%s\n",address.UTF8String,(long)status,error.localizedDescription.UTF8String ?: "HTTP failure");
  dispatch_semaphore_signal(ready);
 }] resume];
 dispatch_semaphore_wait(ready,DISPATCH_TIME_FOREVER);[session finishTasksAndInvalidate];return result;
}
