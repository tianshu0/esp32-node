// UI 图标生成：SVG -> LVGL 9 C 位图数组（ARGB8888）
//
// 位置：main/assets/scripts/gen_assets.js（开发工具，不参与固件编译）
// 用法：cd scripts && npm install && node gen_assets.js
// 生成物：main/assets/src/ui_icons.c / ui_icons.h（被固件 CMake 直接编译）
//
// LVGL 9 ARGB8888 内存字节序为 [B, G, R, A]（小端 uint32 0xAARRGGBB）
const { Resvg } = require('@resvg/resvg-js');
const fs = require('fs');
const path = require('path');

const SRC_DIR = path.join(__dirname, '..');          // main/assets（SVG 源）
const OUT_DIR = path.join(__dirname, '..', 'src');   // main/assets/src（生成物）

// 图标清单：源 SVG -> LVGL 图像符号（尺寸与界面占位一致）
const ICONS = [
    { svg: '房子.svg', name: 'house', size: 18 },
    { svg: '风扇 (1).svg', name: 'fan', size: 20 },
];

function renderIcon(svgFile, size) {
    const data = fs.readFileSync(path.join(SRC_DIR, svgFile), 'utf8');
    const resvg = new Resvg(data, {
        fitTo: { mode: 'width', value: size },
        font: { loadSystemFonts: false },
    });
    const rendered = resvg.render();
    return { w: rendered.width, h: rendered.height, pixels: rendered.pixels };
}

// RGBA(resvg 输出) -> LVGL ARGB8888 字节序 [B, G, R, A]
function rgbaToArgb8888(pixels) {
    const bytes = [];
    for (let i = 0; i < pixels.length; i += 4) {
        bytes.push(pixels[i + 2], pixels[i + 1], pixels[i], pixels[i + 3]);
    }
    return bytes;
}

function formatBytes(bytes, indent) {
    const lines = [];
    for (let i = 0; i < bytes.length; i += 12) {
        const row = bytes.slice(i, i + 12)
            .map(b => '0x' + b.toString(16).padStart(2, '0'))
            .join(', ');
        lines.push(indent + row + ',');
    }
    return lines.join('\n');
}

function main() {
    fs.mkdirSync(OUT_DIR, { recursive: true });

    const headerLines = [
        '// 自动生成：main/assets/scripts/gen_assets.js，勿手改',
        '#pragma once',
        '',
        '#include <lvgl.h>',
        '',
        '#ifdef __cplusplus',
        'extern "C" {',
        '#endif',
        '',
    ];
    const srcLines = [
        '// 自动生成：main/assets/scripts/gen_assets.js，勿手改',
        '#include "ui_icons.h"',
        '',
    ];

    for (const icon of ICONS) {
        console.log(`rendering ${icon.svg} -> ${icon.size}x${icon.size}`);
        const img = renderIcon(icon.svg, icon.size);
        if (img.w !== icon.size || img.h !== icon.size) {
            throw new Error(`${icon.svg}: got ${img.w}x${img.h}, want ${icon.size}x${icon.size}`);
        }
        const bytes = rgbaToArgb8888(img.pixels);

        headerLines.push(`extern const lv_image_dsc_t img_${icon.name};`);
        srcLines.push(
            `static const uint8_t img_${icon.name}_data[] = {`,
            formatBytes(bytes, '    '),
            '};',
            '',
            `const lv_image_dsc_t img_${icon.name} = {`,
            '    .header.magic = LV_IMAGE_HEADER_MAGIC,',
            '    .header.cf = LV_COLOR_FORMAT_ARGB8888,',
            `    .header.w = ${img.w},`,
            `    .header.h = ${img.h},`,
            `    .header.stride = ${img.w} * 4,`,
            '    .data_size = sizeof(img_' + icon.name + '_data),',
            '    .data = img_' + icon.name + '_data,',
            '};',
            '',
        );
        console.log(`OK: img_${icon.name} ${img.w}x${img.h} (${bytes.length} B)`);
    }

    headerLines.push('#ifdef __cplusplus', '}', '#endif', '');

    fs.writeFileSync(path.join(OUT_DIR, 'ui_icons.h'), headerLines.join('\n'));
    fs.writeFileSync(path.join(OUT_DIR, 'ui_icons.c'), srcLines.join('\n'));
    console.log(`\nwritten: ${path.join(OUT_DIR, 'ui_icons.h')} / ui_icons.c`);
}

main();
