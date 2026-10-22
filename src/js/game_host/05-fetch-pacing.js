// 传输槽只约束 native URLSession 在途任务；下载完成即补下一条请求。Response 交给 Pixi 的
// 时机仍由 __ioQ 分帧控制，因此 CDN 能持续跑满，ZIP/spine/json 解析也不会重新扎堆。
// __FETCH_MAX=0 关闭限制；默认 8 路，对 HTTP/2 CDN 比旧的 4 路更不容易产生空转。
(function(){
  var _f = adapter.fetch, inFlight = 0, queue = [];
  var pump = function(){
    var MAX = (globalThis.__FETCH_MAX!==undefined) ? globalThis.__FETCH_MAX : 8;
    if (MAX <= 0){ while(queue.length){ inFlight++; queue.shift()(); } return; }   // 关闭:全放行(仍 inFlight++ 配平完成回调的 --,否则运行中改回 >0 时计数变负)
    while (inFlight < MAX && queue.length){ var job = queue.shift(); inFlight++; job(); }
  };
  adapter.fetch = function(url, opts){
    return new Promise(function(resolve, reject){
      queue.push(function(){
        var task,released=false;
        var release=function(){
          if(released)return;released=true;inFlight=Math.max(0,inFlight-1);pump();
        };
        try{task=_f.call(adapter, url, opts);}
        catch(error){release();reject(error);return;}
        // Native async fetch exposes an early transport signal. Other adapters keep the original
        // behavior and release when their Promise settles.
        var transport=task&&task.__nativeTransportDone?task.__nativeTransportDone:task;
        Promise.resolve(transport).then(release,release);
        Promise.resolve(task).then(resolve,reject);
      });
      pump();
    });
  };
})();
