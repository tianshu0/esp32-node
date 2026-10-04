// Generate fonts - all in one node script, no shell encoding mess
//
// 位置：scripts/gen_fonts.js（开发工具，不参与固件编译）
// 用法：cd scripts && npm install && node gen_fonts.js
// 生成物：main/display/fonts/lv_font_zh*.c（被固件 CMake 直接编译）
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const FONT_OUT_DIR = path.join(__dirname, '..', 'src');
const DISPLAY_DIR = path.join(FONT_OUT_DIR, '..');  // 子进程 cwd（components/fonts），使 -o 用稳定相对路径
const LV_FONT_CONV = path.join(__dirname, 'node_modules', 'lv_font_conv', 'lv_font_conv.js');
const SOURCE_TTF = 'C:/Windows/Fonts/simhei.ttf';

// Hardcode symbols as a JS string (no file I/O encoding risk)
const SYMS = '环境监测节点风扇控制湿度正常已开启已关闭运行中已停止偏高状态开关自动规则手动设置模式等待连接蓝牙网络时间日期周日快笼子温度启闭高湿正连网周期设待启关闭运行中停止偏蓝牙等环测节温自规控备模未一二三四五六智能通风守护健康·在线配点击甲醛';

console.log('SYMS.length =', SYMS.length);
console.log('feng cp: 0x' + '风'.codePointAt(0).toString(16));
console.log('shan cp: 0x' + '扇'.codePointAt(0).toString(16));

const runs = [
    { size: 8,  file: 'lv_font_zh8.c',  name: 'lv_font_zh8'  },
    { size: 10, file: 'lv_font_zh10.c', name: 'lv_font_zh10' },
    { size: 12, file: 'lv_font_zh12.c', name: 'lv_font_zh12' },
    { size: 14, file: 'lv_font_zh14.c', name: 'lv_font_zh14' },
    { size: 16, file: 'lv_font_zh16.c', name: 'lv_font_zh16' },
    { size: 24, file: 'lv_font_zh24.c', name: 'lv_font_zh24' },
];

async function run() {
    for (const r of runs) {
        // -o 用相对 DISPLAY_DIR 的路径，保证生成文件头 Opts 注释不含机器相关绝对路径
        const relOut = 'src/' + r.file;
        const outFile = path.join(FONT_OUT_DIR, r.file);
        await new Promise((resolve, reject) => {
            console.log(`\n=== generating ${outFile} ===`);
            const cpRanges = SYMS.split('').map(c => '0x' + c.codePointAt(0).toString(16)).join(',');
            const args = [
                LV_FONT_CONV,
                '--font', SOURCE_TTF,
                '--size', String(r.size),
                '--bpp', '4',
                '--format', 'lvgl',
                '--lv-font-name', r.name,
                '--lv-include', 'lvgl.h',
                '--no-compress',
                '-r', '0x20-0x7F',
                '-r', '0xB0',
                '-r', cpRanges,
                '-o', relOut,
            ];
            const cp = spawn(process.execPath, args, { cwd: DISPLAY_DIR, stdio: 'inherit' });
            cp.on('exit', code => {
                if (code === 0) {
                    console.log(`OK: ${outFile}`);
                    resolve();
                } else {
                    reject(new Error(`exit ${code}`));
                }
            });
            cp.on('error', reject);
        });
    }
    // Verify using correct LVGL sparse_tiny math (real_cp = range_start + offset)
    console.log('\n=== verification ===');
    const src = fs.readFileSync(path.join(FONT_OUT_DIR, 'lv_font_zh16.c'), 'utf8');
    // Extract unicode_list_1 offsets
    const m = src.match(/unicode_list_1\[\] = \{[\s\S]*?\};/);
    const offsets = [...m[0].matchAll(/0x([0-9a-fA-F]+)/g)].map(x => parseInt(x[1], 16));
    // Find the cmap entry that has .unicode_list = unicode_list_1, then read range_start
    const cmapBlock = src.match(/\{[^}]*\.unicode_list\s*=\s*unicode_list_1[^}]*\}/s);
    const rsMatch = cmapBlock[0].match(/\.range_start\s*=\s*(\d+)/);
    const rangeStart = parseInt(rsMatch[1]);
    console.log('cmap range_start:', rangeStart, '(0x' + rangeStart.toString(16) + ')');
    const realCodes = new Set(offsets.map(o => rangeStart + o));
    const targets = ['风','扇','控','制','湿','度','正','常','已','开','启','关','闭','运','行','中','停','止','偏','高','状','态','连','接','网','络','时','间','日','期','周','蓝','牙','未','一','二','三','四','五','六','·','在','线','配'];
    let ok = 0;
    for (const ch of targets) {
        const cp = ch.codePointAt(0);
        if (realCodes.has(cp)) ok++;
        else console.log('MISS: ' + ch + ' U+' + cp.toString(16).toUpperCase());
    }
    console.log('Result: ' + ok + '/' + targets.length + ' present');
    if (ok < targets.length) { console.error('ERROR: some chars missing!'); process.exit(1); }
    else console.log('All target chars present!');
}

run().catch(e => { console.error(e); process.exit(1); });
