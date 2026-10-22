// Slot lease contract — the single source of truth for how a JS shell acquires, migrates and
// loses its SceneGraph slot (docs/shadow-elimination-plan.md §3, v2).
//
// A lease is the right to read/write a shared Float32 slot as the object's local-transform
// truth. It is taken and lost through four ordered actions:
//   capture   before losing/moving a binding: snapshot the current truth (slot or stash) into
//             the stash fields — NEVER after the new offset is visible to getters.
//   detach    invalidate the old lease (__o/__sgOwner/__sgLease) so accessors fall back to stash.
//   populate  write the captured values into the newly claimed slot.
//   activate  only then expose the slot to readers (set __sgLease last).
// The orchestrators below own that order. Hosts inject IO callbacks; tests inject recorders.
// Both run the same code path, so the order cannot silently regress.
//
// Ownership rule: capture may read the slot ONLY while the caller still owns it. A shell whose
// registry entry no longer matches (stale owner) must capture from its stash — the slot already
// belongs to someone else (captureSource / runRelease(ownsSlot)).
(function(root,factory){
'use strict';
var api=factory();
if(typeof module==='object'&&module&&module.exports)module.exports=api;
if(root)root.__nativeSlotLeaseContract=api;
}(typeof globalThis!=='undefined'?globalThis:this,function(){
'use strict';
var ST=48;
var fround=Math.fround;

// getter gate: slot may be read only under an ACTIVE lease. Three conditions, all required:
//   1. generation matches (__sgLease === owner.__sgBindGeneration) — set last (activate), so a
//      populated-but-not-activated slot is invisible, and any real ownership claim expires every
//      earlier lease (same-id re-leases, stale points kept by replaced Transforms).
//   2. owner.__i still points at this very slot.
//   3. the registry still lists this owner (registry[id] === owner) — generation and registry
//      ownership are complementary: generation catches object re-leases, the registry catches
//      out-of-band takeovers (native factories); neither substitutes for the other.
// `registry` is injected to keep this pure; omit it only in registry-free unit contexts.
function canReadSlot(pt,on,registry){
  if(on!==1||!pt)return false;
  var off=pt.__o;if(off===undefined||pt.__sgLease===undefined)return false;
  var ow=pt.__sgOwner;if(!ow)return false;
  var id=(off/ST)|0;
  if(ow.__sgBindGeneration!==pt.__sgLease||(ow.__i|0)!==id)return false;
  return registry?registry[id]===ow:true;
}

// write normalisation: inside an active lease the store is Float32, so equality checks must run
// in Float32 semantics (pre-fround). Outside a lease values stay double — unconditionally
// frounding would degrade unbound points / tileTransform (plan §3.7).
function writeValue(v,bound){return bound?fround(v):v;}

// capture source: 'slot' only while the slot is still ours; otherwise stash (plan §3.2).
function captureSource(ownsSlot){return ownsSlot?'slot':'stash';}

function activateFields(pt,off,owner){
  pt.__o=off;pt.__sgOwner=owner||null;
  pt.__sgLease=owner?owner.__sgBindGeneration:undefined;
}
function detachFields(pt){
  if(!pt)return;
  pt.__o=undefined;pt.__sgOwner=null;pt.__sgLease=undefined;
}

// bind: unbound (or rebinding) shell takes a slot.
//   io.capture()        -> captured local values, read from the CURRENT truth (stash fields;
//                          no __o has been touched yet, so getters still serve the stash)
//   io.claim()          -> allocate/claim the slot, wire offsets (__b/__o), install accessors;
//                          leases stay unset, so getters keep serving the stash
//   io.populate(vals)   -> write the captured values into the new slot
//   io.activate()       -> set leases; from here getters read the slot
function runBind(io){
  var vals=io.capture();
  if(io.claim()===false)return false;   // alloc failure: nothing was activated
  io.populate(vals);
  io.activate();
  return true;
}

// rebind: FGUI virtual lists replace Transform/point objects after the id exists.
//   io.revokeOld() must detach every previously leased point that is no longer current —
//   a replaced point keeps its old offset forever otherwise (plan §3.2).
function runRebind(io){
  var vals=io.capture();
  io.revokeOld();
  io.rewire();
  io.populate(vals);
  io.activate();
  return true;
}

// release: destroy / claim-displacement / ownership mismatch.
//   ownsSlot=false (stale owner) never touches the slot: stash already holds the last
//   business truth. captureFromSlot runs only under single-source reads (gated by caller):
//   with the switch off, copying slot values into the stash would CHANGE what shadows
//   return today (plan §7 phase-0 gate 4).
function runRelease(io,ownsSlot,singleSourceOn){
  if(ownsSlot&&singleSourceOn===1)io.captureFromSlot();
  io.detach();
  return true;
}

return {
  ST:ST,
  canReadSlot:canReadSlot,
  writeValue:writeValue,
  captureSource:captureSource,
  activateFields:activateFields,
  detachFields:detachFields,
  runBind:runBind,
  runRebind:runRebind,
  runRelease:runRelease
};
}));
