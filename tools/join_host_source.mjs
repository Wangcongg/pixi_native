#!/usr/bin/env node
// join_host_source.mjs — 把 src/js/<unit>/ 下按段拆分的源码片段,按 manifest 顺序原样拼接成 game/ 下的运行时文件。
//
// 为什么是"拼接"而不是"多文件加载":game_host.js 与 native_scenegraph.js 各自是一个有意为之的
// IIFE 闭包(前者防内部变量泄漏成全局被压缩库的单字母全局覆盖;后者 345 个闭包级声明、144 个跨段共享),
// 物理拆成多个独立求值的脚本会改变作用域。拼接结果与拆分前逐字节一致,运行时/打包/热更新/测试零变化。
//
// 用法:
//   node tools/join_host_source.mjs          重新生成所有 unit 的输出文件
//   node tools/join_host_source.mjs --check  只校验:输出文件是否等于片段拼接结果(ctest 漂移守卫)
//   node tools/join_host_source.mjs <unit>   只处理一个 unit(如 game_host)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = path.join(projectRoot, 'src', 'js');
const args = process.argv.slice(2);
const checkOnly = args.includes('--check');
const only = args.filter((a) => !a.startsWith('--'));

function loadUnit(unit) {
  const dir = path.join(sourceRoot, unit);
  const manifestPath = path.join(dir, 'manifest.json');
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (typeof manifest.output !== 'string' || !Array.isArray(manifest.parts) || manifest.parts.length === 0) {
    throw new Error(`${manifestPath}: 需要 {output: string, parts: string[]}`);
  }
  const chunks = manifest.parts.map((name) => {
    const file = path.join(dir, name);
    if (!fs.existsSync(file)) throw new Error(`${unit}: 片段缺失 ${name}`);
    return fs.readFileSync(file);
  });
  return { unit, dir, output: path.join(projectRoot, manifest.output), outputRel: manifest.output, joined: Buffer.concat(chunks), parts: manifest.parts };
}

const units = fs.readdirSync(sourceRoot, { withFileTypes: true })
  .filter((d) => d.isDirectory() && fs.existsSync(path.join(sourceRoot, d.name, 'manifest.json')))
  .map((d) => d.name)
  .filter((u) => only.length === 0 || only.includes(u));
if (units.length === 0) { console.error('未找到任何 unit(src/js/<unit>/manifest.json)'); process.exit(2); }

let drift = 0;
for (const unit of units.map(loadUnit)) {
  const current = fs.existsSync(unit.output) ? fs.readFileSync(unit.output) : null;
  const same = current !== null && current.equals(unit.joined);
  if (checkOnly) {
    if (same) {
      console.log(`ok    ${unit.outputRel} == join(${unit.parts.length} parts, ${unit.joined.length} bytes)`);
    } else {
      drift += 1;
      console.error(`DRIFT ${unit.outputRel} != join(src/js/${unit.unit}/*)  →  运行 node tools/join_host_source.mjs ${unit.unit}`);
      if (current !== null) {
        // 定位首个差异字节,方便判断是改了输出文件还是改了片段。
        const n = Math.min(current.length, unit.joined.length);
        let i = 0; while (i < n && current[i] === unit.joined[i]) i += 1;
        const line = unit.joined.subarray(0, i).toString('utf8').split('\n').length;
        console.error(`      首个差异在输出文件第 ${line} 行附近(输出 ${current.length} 字节,拼接 ${unit.joined.length} 字节)`);
      }
    }
    continue;
  }
  if (same) { console.log(`unchanged ${unit.outputRel}`); continue; }
  fs.writeFileSync(unit.output, unit.joined);
  console.log(`wrote     ${unit.outputRel} (${unit.parts.length} parts, ${unit.joined.length} bytes)`);
}
if (drift) process.exit(1);
