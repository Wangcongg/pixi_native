#pragma once
// quickjs_intl_polyfill.h — QuickJS 没有 ECMA-402 Intl;Android 上 JS 启动在 pixi.v7.4.3.js 就以 "'Intl' is not defined" 失败
// 本补丁只在 typeof Intl === 'undefined' 时安装,覆盖 en-US 的 DateTimeFormat(数字/文本月、12/24 小时、时区 UTC/美区五区/上海,
// 美国夏令时规则)、NumberFormat(分组/小数位/ICU 最短表示 halfExpand 舍入)、Collator/PluralRules 桩;另在宿主的
// Number.prototype.toLocaleString 不分组时(QuickJS 就是 toString)把它接成 NumberFormat 语义。语义以 node(ICU)真 Intl 为 oracle:
// tests/quickjs_intl_polyfill_test.mjs 逐字比对 format 与 formatToParts(含 ICU 的 24 小时制午夜 "24" 习惯)。
// 工厂 __quickjsIntlPolyfillFactory 与 __quickjsIntlPolyfillNumberToLocaleString 暴露给测试(node 自带 Intl 时也能对拍)。
// 本文件只有 JS 源码,不依赖引擎:jsc_quickjs_shim.h / jsc_v8_shim.h 各自在上下文创建后、任何游戏脚本之前求值一次
// (V8 带 ICU 时 typeof Intl !== 'undefined' 且 toLocaleString 已分组,补丁自动不装)。
#include <cstring>

static const char* kQuickJsIntlPolyfill = R"JS(
(function(){
  'use strict';
  function makeIntl(){
    var MONTHS_LONG=['January','February','March','April','May','June','July','August','September','October','November','December'];
    var MONTHS_SHORT=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    var DAYS_LONG=['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
    var DAYS_SHORT=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
    var ZONES={
      'UTC':{std:0,dst:null,names:['UTC','UTC']},'Etc/UTC':{std:0,dst:null,names:['UTC','UTC']},'GMT':{std:0,dst:null,names:['GMT','GMT']},
      'America/Phoenix':{std:-420,dst:null,names:['MST','MST']},
      'America/Los_Angeles':{std:-480,dst:'us',names:['PST','PDT']},'America/Denver':{std:-420,dst:'us',names:['MST','MDT']},
      'America/Chicago':{std:-360,dst:'us',names:['CST','CDT']},'America/New_York':{std:-300,dst:'us',names:['EST','EDT']},
      'Asia/Shanghai':{std:480,dst:null,names:['GMT+8','GMT+8']},'Asia/Tokyo':{std:540,dst:null,names:['GMT+9','GMT+9']}
    };
    function nthSunday(year,month,n){var dow=new Date(Date.UTC(year,month,1)).getUTCDay();return 1+((7-dow)%7)+(n-1)*7;}
    function usDstActive(utcMs,std){
      var year=new Date(utcMs+std*60000).getUTCFullYear();
      var start=Date.UTC(year,2,nthSunday(year,2,2),2,0,0)-std*60000;        // 二月第二个周日 2:00 标准时
      var end=Date.UTC(year,10,nthSunday(year,10,1),2,0,0)-(std+60)*60000;   // 十一月第一个周日 2:00 夏令时
      return utcMs>=start&&utcMs<end;
    }
    function zoneAt(tz,utcMs){
      if(!tz)return null;                                   // null = 本地时区
      var z=ZONES[tz];if(!z)return {offset:0,name:'UTC'};   // 未知时区退化为 UTC(确定性优先)
      if(z.dst==='us'&&usDstActive(utcMs,z.std))return {offset:z.std+60,name:z.names[1]};
      return {offset:z.std,name:z.names[0]};
    }
    function pad2(n){return n<10?'0'+n:''+n;}
    function DateTimeFormat(locale,options){
      if(!(this instanceof DateTimeFormat))return new DateTimeFormat(locale,options);
      var o=options||{};
      var fields={weekday:o.weekday,year:o.year,month:o.month,day:o.day,hour:o.hour,minute:o.minute,second:o.second};
      if(!fields.weekday&&!fields.year&&!fields.month&&!fields.day&&!fields.hour&&!fields.minute&&!fields.second){fields.year='numeric';fields.month='numeric';fields.day='numeric';}
      this._fields=fields;this._hour12=o.hour12===undefined?true:!!o.hour12;this._tz=o.timeZone?String(o.timeZone):null;
      this._tzName=o.timeZoneName?String(o.timeZoneName):null;this._locale=locale?String(Array.isArray(locale)?locale[0]:locale):'en-US';
    }
    DateTimeFormat.prototype.resolvedOptions=function(){return {locale:this._locale,calendar:'gregory',numberingSystem:'latn',timeZone:this._tz||'UTC',hour12:this._hour12};};
    DateTimeFormat.prototype.formatToParts=function(date){
      var ms=date===undefined?Date.now():(date instanceof Date?date.getTime():Number(date));
      if(!isFinite(ms))throw new RangeError('Invalid time value');
      var f=this._fields,zone=zoneAt(this._tz,ms),d=new Date(ms),Y,M,D,W,h,m,s,tzName;
      if(zone){var l=new Date(ms+zone.offset*60000);Y=l.getUTCFullYear();M=l.getUTCMonth();D=l.getUTCDate();W=l.getUTCDay();h=l.getUTCHours();m=l.getUTCMinutes();s=l.getUTCSeconds();tzName=zone.name;}
      else{Y=d.getFullYear();M=d.getMonth();D=d.getDate();W=d.getDay();h=d.getHours();m=d.getMinutes();s=d.getSeconds();var off=-d.getTimezoneOffset();tzName='GMT'+(off===0?'':(off>0?'+':'-')+Math.floor(Math.abs(off)/60)+(Math.abs(off)%60?':'+pad2(Math.abs(off)%60):''));}
      var parts=[];function push(type,value){parts.push({type:type,value:String(value)});}function lit(v){parts.push({type:'literal',value:v});}
      var textMonth=f.month==='short'||f.month==='long'||f.month==='narrow';
      var monthText=f.month==='long'?MONTHS_LONG[M]:f.month==='narrow'?MONTHS_SHORT[M].charAt(0):MONTHS_SHORT[M];
      var dateParts=[];
      if(f.weekday)dateParts.push(['weekday',f.weekday==='long'?DAYS_LONG[W]:f.weekday==='narrow'?DAYS_SHORT[W].charAt(0):DAYS_SHORT[W]]);
      var dp=[];
      if(f.month)dp.push(['month',textMonth?monthText:(f.month==='2-digit'?pad2(M+1):String(M+1))]);
      if(f.day)dp.push(['day',f.day==='2-digit'?pad2(D):String(D)]);
      if(f.year)dp.push(['year',f.year==='2-digit'?pad2(Y%100):String(Y)]);
      var hasDate=dp.length>0,hasTime=!!(f.hour||f.minute||f.second);
      if(dateParts.length){push('weekday',dateParts[0][1]);if(hasDate)lit(', ');}
      for(var i=0;i<dp.length;i++){
        if(i>0){
          var prev=dp[i-1][0],cur=dp[i][0];
          if(textMonth){lit(prev==='day'&&cur==='year'?', ':' ');}   // "Sep 2, 2026" / "Sep 2026" / "Sep 2"
          else lit('/');                                            // "9/2/2026"
        }
        push(dp[i][0],dp[i][1]);
      }
      if(hasTime){
        if(hasDate||dateParts.length)lit(', ');
        var hour12=this._hour12,hv;
        if(f.hour){
          if(hour12){hv=h%12;if(hv===0)hv=12;}else{hv=h===0?24:h;}   // ICU en-US 的 hour12:false 是 h24:午夜显示 24
          push('hour',f.hour==='2-digit'?pad2(hv):String(hv));
        }
        if(f.minute){if(f.hour)lit(':');push('minute',pad2(m));}
        if(f.second){if(f.hour||f.minute)lit(':');push('second',pad2(s));}
        if(f.hour&&hour12){lit('\u202f');push('dayPeriod',h<12?'AM':'PM');}   // ICU parts 用窄不换行空格(U+202F);format() 里 V8 换回普通空格
      }
      if(this._tzName){lit(' ');push('timeZoneName',tzName);}
      return parts;
    };
    DateTimeFormat.prototype.format=function(date){var p=this.formatToParts(date),out='';for(var i=0;i<p.length;i++)out+=p[i].value;return out.replace(/\u202f/g,' ');};
    DateTimeFormat.supportedLocalesOf=function(){return ['en-US'];};
    function NumberFormat(locale,options){
      if(!(this instanceof NumberFormat))return new NumberFormat(locale,options);
      var o=options||{};this._min=o.minimumFractionDigits===undefined?0:o.minimumFractionDigits;
      this._max=o.maximumFractionDigits===undefined?Math.max(this._min,3):o.maximumFractionDigits;this._grouping=o.useGrouping!==false;
    }
    // 数字:ICU 先取 double 的最短往返十进制表示(与 String(n) 同一算法),再按 maximumFractionDigits 做 halfExpand 舍入。
    // 不能用 toFixed——它按二进制精确值舍入:(1.005).toFixed(2)="1.00",ICU/JSC 给 "1.01";大数还会落成指数记法。
    function decimalDigits(abs){   // 有限 abs>=0 → {ip:整数位串, fp:小数位串},展开指数记法
      var s=String(abs),e=s.indexOf('e'),mant=e<0?s:s.slice(0,e),exp=e<0?0:parseInt(s.slice(e+1),10);
      var dot=mant.indexOf('.'),ip=dot<0?mant:mant.slice(0,dot),fp=dot<0?'':mant.slice(dot+1);
      if(exp>0){if(fp.length<=exp){ip+=fp+'0'.repeat(exp-fp.length);fp='';}else{ip+=fp.slice(0,exp);fp=fp.slice(exp);}}
      else if(exp<0){var k=-exp;if(ip.length<=k){fp='0'.repeat(k-ip.length)+ip+fp;ip='0';}else{fp=ip.slice(ip.length-k)+fp;ip=ip.slice(0,ip.length-k);}}
      ip=ip.replace(/^0+(?=\d)/,'');
      return {ip:ip,fp:fp};
    }
    function roundHalfExpand(d,max){   // 小数位截到 max 位;被丢弃的首位 >= 5 即进位(绝对值域内 = 远离零)
      if(d.fp.length<=max)return d;
      var digits=(d.ip+d.fp.slice(0,max)).split(''),up=d.fp.charCodeAt(max)>=53,i=digits.length-1;
      if(up){for(;i>=0;i--){if(digits[i]==='9')digits[i]='0';else{digits[i]=String.fromCharCode(digits[i].charCodeAt(0)+1);break;}}if(i<0)digits.unshift('1');}
      var all=digits.join('');
      return {ip:all.slice(0,all.length-max),fp:all.slice(all.length-max)};
    }
    NumberFormat.prototype.format=function(n){
      n=Number(n);if(!isFinite(n))return n!==n?'NaN':(n<0?'-∞':'∞');
      var neg=n<0||(n===0&&1/n<0),d=roundHalfExpand(decimalDigits(Math.abs(n)),this._max),ip=d.ip,fp=d.fp;
      while(fp.length<this._min)fp+='0';
      while(fp.length>this._min&&fp.charAt(fp.length-1)==='0')fp=fp.slice(0,-1);
      if(this._grouping)ip=ip.replace(/\B(?=(\d{3})+(?!\d))/g,',');
      return (neg?'-':'')+ip+(fp?'.'+fp:'');   // ICU 保留负号(-0.004 → "-0.00",-0 → "-0")
    };
    NumberFormat.prototype.resolvedOptions=function(){return {locale:'en-US',numberingSystem:'latn',minimumFractionDigits:this._min,maximumFractionDigits:this._max,useGrouping:this._grouping};};
    function Collator(){if(!(this instanceof Collator))return new Collator();}
    Collator.prototype.compare=function(a,b){a=String(a);b=String(b);return a<b?-1:a>b?1:0;};
    function PluralRules(){if(!(this instanceof PluralRules))return new PluralRules();}
    PluralRules.prototype.select=function(n){return Number(n)===1?'one':'other';};
    return {DateTimeFormat:DateTimeFormat,NumberFormat:NumberFormat,Collator:Collator,PluralRules:PluralRules,getCanonicalLocales:function(l){return l?[].concat(l):[];}};
  }
  var installed=null;
  if(typeof Intl==='undefined'){installed=makeIntl();globalThis.Intl=installed;}
  // bellard QuickJS 的 Number.prototype.toLocaleString 就是十进制 toString(quickjs.c js_number_toString magic=1),不分组:
  // 只在宿主实现不分组时接管(node/JSC 的 ICU 实现不动),语义 = en-US Intl.NumberFormat;非 Number 接收者按规范抛 TypeError。
  var NumberFormatImpl=(installed||makeIntl()).NumberFormat;
  function numberToLocaleString(locales,options){
    var n=Number.prototype.valueOf.call(this);
    return new NumberFormatImpl(locales,options).format(n);
  }
  var numberNeedsPatch=false;
  try{numberNeedsPatch=(1000).toLocaleString()!=='1,000';}catch(e){numberNeedsPatch=true;}
  if(numberNeedsPatch)Object.defineProperty(Number.prototype,'toLocaleString',{value:numberToLocaleString,writable:true,configurable:true,enumerable:false});
  globalThis.__quickjsIntlPolyfillFactory=makeIntl;
  globalThis.__quickjsIntlPolyfillNumberToLocaleString=numberToLocaleString;
})();
)JS";
