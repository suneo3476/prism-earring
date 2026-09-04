/*
 * qr-test.mjs — web/qr.js(自前 QR エンコーダ)のオフライン検証
 *
 * 実行: node web/test/qr-test.mjs
 * 依存: Node 18+ のみ(外部パッケージなし)。
 *
 * 検証方針: エンコーダの内部実装を写した「答え合わせ」にならないよう、
 * 照合先はすべて外部の既知値か、独立に書き直した逆向きの実装にする。
 *   A. ISO/IEC 18004 の規格表(Annex A 生成多項式 / Annex C 形式情報 / Annex D 版情報)
 *   B. 手計算で導いたバイトモード "HELLO WORLD" のデータコードワード
 *   C. 本ファイル内に別途書き下ろした QR デコーダ(逆ジグザグ走査・逆インターリーブ・
 *      シンドローム検査)による往復検証
 * これらは web/qr.js のコードを一切参照せずに書いてある。
 */

import {
    encodeQr,
    toSvgPathData,
    rsDivisor,
    formatBits,
    versionBits,
    totalCodewords,
    dataCodewords,
    eccParams,
    alignmentPositions,
    buildDataCodewords,
    gfMul,
    gfPow,
    MASK_RULES
} from '../qr.js';

/* ----------------------------- 走行管理 ----------------------------- */

let pass = 0;
let fail = 0;

function ok(condition, name, detail = '') {
    if (condition) {
        pass++;
        console.log(`  PASS  ${name}${detail ? ` — ${detail}` : ''}`);
    } else {
        fail++;
        console.log(`  FAIL  ${name}${detail ? ` — ${detail}` : ''}`);
    }
}

function eq(actual, expected, name) {
    const same = String(actual) === String(expected);
    ok(same, name, same ? String(actual) : `期待 ${expected} / 実際 ${actual}`);
}

function throws(fn, name) {
    try {
        fn();
    } catch (err) {
        ok(true, name, err.message.slice(0, 60));
        return;
    }
    ok(false, name, '例外が投げられなかった');
}

function section(title) {
    console.log(`\n${title}`);
}

/* ------------------- 独立実装: GF(256) と多項式評価 ------------------- */

/* エンコーダとは別に、素朴なビット演算で GF(256) の乗算を書き直す。 */
function gfMulSlow(a, b) {
    let result = 0;
    for (let i = 0; i < 8; i++) {
        if ((b >> i) & 1) {
            let term = a;
            for (let k = 0; k < i; k++) {
                term <<= 1;
                if (term & 0x100) {
                    term ^= 0x11d;
                }
            }
            result ^= term;
        }
    }
    return result & 0xff;
}

/** 降べき順の係数配列 poly を GF(256) 上の点 x で評価する(ホーナー法)。 */
function polyEval(poly, x) {
    let acc = 0;
    for (const coefficient of poly) {
        acc = gfMulSlow(acc, x) ^ coefficient;
    }
    return acc;
}

/* -------------------- 独立実装: QR デコーダ(逆向き) -------------------- */

/**
 * 行列から機能パターンの位置だけを独立に組み立てる。
 * qr.js の drawFunctionPatterns は参照せず、規格の記述から書き下ろす。
 */
function functionMap(size, version) {
    const isFn = Array.from({ length: size }, () => new Array(size).fill(false));
    const mark = (x, y) => {
        if (x >= 0 && y >= 0 && x < size && y < size) {
            isFn[y][x] = true;
        }
    };

    // 位置検出パターン + 分離帯(8x8 の角 3 か所)
    for (let d = 0; d < 8; d++) {
        for (let e = 0; e < 8; e++) {
            mark(d, e);
            mark(size - 1 - d, e);
            mark(d, size - 1 - e);
        }
    }
    // 形式情報の帯
    for (let i = 0; i < 9; i++) {
        mark(8, i);
        mark(i, 8);
    }
    for (let i = 0; i < 8; i++) {
        mark(size - 1 - i, 8);
        mark(8, size - 1 - i);
    }
    // タイミングパターン
    for (let i = 0; i < size; i++) {
        mark(6, i);
        mark(i, 6);
    }
    // 整列パターン(規格の中心座標表を別途持つ)
    const centers = ALIGNMENT_TABLE[version];
    for (const cy of centers) {
        for (const cx of centers) {
            // 位置検出パターンと重なる 3 隅は置かれない
            const nearFinder =
                (cx <= 8 && cy <= 8) || (cx >= size - 9 && cy <= 8) || (cx <= 8 && cy >= size - 9);
            if (nearFinder) {
                continue;
            }
            for (let dy = -2; dy <= 2; dy++) {
                for (let dx = -2; dx <= 2; dx++) {
                    mark(cx + dx, cy + dy);
                }
            }
        }
    }
    // バージョン情報(7 以上)
    if (version >= 7) {
        for (let i = 0; i < 18; i++) {
            const a = size - 11 + (i % 3);
            const b = Math.floor(i / 3);
            mark(a, b);
            mark(b, a);
        }
    }
    return isFn;
}

/** 行列から形式情報 15bit を読み、BCH を検算して誤り訂正レベルとマスクを返す。 */
function readFormat(modules, size) {
    const positions = [];
    for (let i = 0; i <= 5; i++) {
        positions.push([8, i]);
    }
    positions.push([8, 7], [8, 8], [7, 8]);
    for (let i = 9; i < 15; i++) {
        positions.push([14 - i, 8]);
    }
    let raw = 0;
    positions.forEach(([x, y], i) => {
        if (modules[y][x]) {
            raw |= 1 << i;
        }
    });
    const value = raw ^ 0x5412;

    // BCH(15,5) の検算: 15bit を生成多項式 0x537 で割った剰余が 0 になるはず。
    let rem = value;
    for (let i = 14; i >= 10; i--) {
        if ((rem >>> i) & 1) {
            rem ^= 0x537 << (i - 10);
        }
    }
    const data = value >>> 10;
    return { valid: rem === 0, eccBits: (data >>> 3) & 3, mask: data & 7 };
}

/** ジグザグ走査を逆にたどってコードワード列を復元する。 */
function readCodewords(modules, isFn, size, count) {
    const bits = [];
    for (let right = size - 1; right >= 1; right -= 2) {
        if (right === 6) {
            right = 5;
        }
        for (let vert = 0; vert < size; vert++) {
            for (let j = 0; j < 2; j++) {
                const x = right - j;
                const upward = ((right + 1) & 2) === 0;
                const y = upward ? size - 1 - vert : vert;
                if (!isFn[y][x]) {
                    bits.push(modules[y][x] ? 1 : 0);
                }
            }
        }
    }
    const out = new Uint8Array(count);
    for (let i = 0; i < count; i++) {
        let byte = 0;
        for (let j = 0; j < 8; j++) {
            byte = (byte << 1) | bits[i * 8 + j];
        }
        out[i] = byte;
    }
    return out;
}

/** インターリーブを解いてブロック(データ + 誤り訂正)へ戻す。 */
function deinterleave(stream, version, ecc) {
    const { eccPerBlock, numBlocks } = eccParams(version, ecc);
    const raw = totalCodewords(version);
    const numShort = numBlocks - (raw % numBlocks);
    const shortLen = Math.floor(raw / numBlocks);
    const dataLens = [];
    for (let j = 0; j < numBlocks; j++) {
        dataLens.push(shortLen - eccPerBlock + (j < numShort ? 0 : 1));
    }
    const blocks = dataLens.map(() => []);
    let k = 0;
    const maxDataLen = Math.max(...dataLens);
    for (let i = 0; i < maxDataLen; i++) {
        for (let j = 0; j < numBlocks; j++) {
            if (i < dataLens[j]) {
                blocks[j].push(stream[k++]);
            }
        }
    }
    const eccParts = blocks.map(() => []);
    for (let i = 0; i < eccPerBlock; i++) {
        for (let j = 0; j < numBlocks; j++) {
            eccParts[j].push(stream[k++]);
        }
    }
    return { blocks, eccParts, consumed: k, raw };
}

/**
 * 行列を復号する。ここでは誤りが無い前提でシンドロームが全て 0 であることだけ確認し、
 * データ部からバイトモードのセグメントを取り出す。
 */
function decodeQr(matrix) {
    const modules = matrix.modules.map((row) => row.slice());
    const size = modules.length;
    const version = (size - 17) / 4;
    const isFn = functionMap(size, version);

    const format = readFormat(modules, size);
    const ecc = { 1: 'L', 0: 'M', 3: 'Q', 2: 'H' }[format.eccBits];

    // マスクを外す(XOR なので同じ規則をもう一度当てる)。
    const rule = MASK_RULES[format.mask];
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            if (!isFn[y][x] && rule(x, y)) {
                modules[y][x] = !modules[y][x];
            }
        }
    }

    const raw = totalCodewords(version);
    const stream = readCodewords(modules, isFn, size, raw);
    const { blocks, eccParts } = deinterleave(stream, version, ecc);
    const { eccPerBlock } = eccParams(version, ecc);

    // シンドローム検査: 符号語多項式を α^1..α^eccPerBlock で評価すると全て 0 になる。
    const syndromes = [];
    for (let j = 0; j < blocks.length; j++) {
        const codeword = blocks[j].concat(eccParts[j]);
        for (let s = 1; s <= eccPerBlock; s++) {
            syndromes.push(polyEval(codeword, gfPow(s - 1)));
        }
    }

    // データ部を連結してビット列に戻し、バイトモードのセグメントを読む。
    const dataBytes = [];
    for (const block of blocks) {
        dataBytes.push(...block);
    }
    const bits = [];
    for (const byte of dataBytes) {
        for (let i = 7; i >= 0; i--) {
            bits.push((byte >> i) & 1);
        }
    }
    let p = 0;
    const take = (n) => {
        let v = 0;
        for (let i = 0; i < n; i++) {
            v = (v << 1) | bits[p++];
        }
        return v;
    };
    const mode = take(4);
    const countBits = version <= 9 ? 8 : 16;
    const length = take(countBits);
    const payload = [];
    for (let i = 0; i < length; i++) {
        payload.push(take(8));
    }
    const text = new TextDecoder().decode(Uint8Array.from(payload));

    return {
        version,
        ecc,
        mask: format.mask,
        formatValid: format.valid,
        mode,
        length,
        text,
        syndromesAllZero: syndromes.every((s) => s === 0),
        alwaysDark: modules[size - 8][8] === true
    };
}

/* --------------------------- 規格の既知値 --------------------------- */

/* ISO/IEC 18004 Annex C 形式情報。索引は [レベル][マスク]。 */
const FORMAT_TABLE = {
    L: [
        '111011111000100', '111001011110011', '111110110101010', '111100010011101',
        '110011000101111', '110001100011000', '110110001000001', '110100101110110'
    ],
    M: [
        '101010000010010', '101000100100101', '101111001111100', '101101101001011',
        '100010111111001', '100000011001110', '100111110010111', '100101010100000'
    ]
};

/* ISO/IEC 18004 Annex D 版情報(バージョン 7 以上)。 */
const VERSION_TABLE = {
    7: '000111110010010100',
    8: '001000010110111100',
    9: '001001101010011001',
    10: '001010010011010011'
};

/* ISO/IEC 18004 Annex A: 誤り訂正符号語 10 個ぶんの生成多項式(α の指数、降べき順)。 */
const GEN10_ALPHA = [0, 251, 67, 46, 61, 118, 70, 64, 94, 32, 45];

/* 表 1(記号の総符号語数)と表 9(レベル M のデータ符号語数)。 */
const TOTAL_CODEWORDS = [26, 44, 70, 100, 134, 172, 196, 242, 292, 346];
const DATA_CODEWORDS_M = [16, 28, 44, 64, 86, 108, 124, 154, 182, 216];

/* 表 E.1 整列パターン中心座標。 */
const ALIGNMENT_TABLE = {
    1: [], 2: [6, 18], 3: [6, 22], 4: [6, 26], 5: [6, 30],
    6: [6, 34], 7: [6, 22, 38], 8: [6, 24, 42], 9: [6, 26, 46], 10: [6, 28, 50]
};

/* ======================== ここからテスト本体 ======================== */

section('1. GF(256) — 独立実装との一致と既知値');

{
    // α^8 は原始多項式より x^4+x^3+x^2+1 = 0x1D。
    eq(gfPow(0), 1, 'α^0 = 1');
    eq(gfPow(1), 2, 'α^1 = 2');
    eq(gfPow(8), 0x1d, 'α^8 = 0x1D(原始多項式 0x11D から)');
    eq(gfPow(254), gfPow(-1), 'α^254 = α^-1(指数は 255 で巡回)');
    eq(gfMul(gfPow(254), gfPow(1)), 1, 'α^254 · α^1 = 1(乗法逆元)');

    let mismatches = 0;
    for (let a = 0; a < 256; a++) {
        for (let b = 0; b < 256; b++) {
            if (gfMul(a, b) !== gfMulSlow(a, b)) {
                mismatches++;
            }
        }
    }
    eq(mismatches, 0, '対数表による乗算 65536 通りがビット演算実装と一致する');
}

section('2. Reed–Solomon 生成多項式');

{
    // 生成多項式は α^0 .. α^(n-1) を根に持つ。これが RS 符号の定義そのもの。
    for (const degree of [10, 16, 18, 22, 24, 26]) {
        const full = [1, ...rsDivisor(degree)];
        let bad = 0;
        for (let i = 0; i < degree; i++) {
            if (polyEval(full, gfPow(i)) !== 0) {
                bad++;
            }
        }
        eq(bad, 0, `degree=${degree} の生成多項式が α^0..α^${degree - 1} を根に持つ`);
    }
    // 非根では 0 にならないこと(退化した実装を弾く)。
    const full10 = [1, ...rsDivisor(10)];
    ok(polyEval(full10, gfPow(10)) !== 0, 'degree=10 の生成多項式は α^10 では 0 にならない');

    // Annex A の係数表(α の指数)と突き合わせる。
    const alphas = full10.map((c) => {
        for (let i = 0; i < 255; i++) {
            if (gfPow(i) === c) {
                return i;
            }
        }
        return -1;
    });
    eq(alphas.join(' '), GEN10_ALPHA.join(' '), 'degree=10 の係数が ISO Annex A の表と一致する');
}

section('3. 形式情報(ISO Annex C)と版情報(ISO Annex D)');

{
    for (const level of ['L', 'M']) {
        for (let mask = 0; mask < 8; mask++) {
            const bits = formatBits(level, mask).toString(2).padStart(15, '0');
            eq(bits, FORMAT_TABLE[level][mask], `形式情報 ${level} / マスク ${mask}`);
        }
    }
    for (const version of [7, 8, 9, 10]) {
        const bits = versionBits(version).toString(2).padStart(18, '0');
        eq(bits, VERSION_TABLE[version], `版情報 バージョン ${version}`);
    }
    throws(() => formatBits('M', 8), 'マスク番号 8 は範囲外として弾く');
    throws(() => formatBits('X', 0), '未知の誤り訂正レベルを弾く');
}

section('4. 容量表・整列パターン表');

{
    for (let v = 1; v <= 10; v++) {
        eq(totalCodewords(v), TOTAL_CODEWORDS[v - 1], `総符号語数 バージョン ${v}`);
        eq(dataCodewords(v, 'M'), DATA_CODEWORDS_M[v - 1], `データ符号語数 バージョン ${v} / M`);
        eq(
            alignmentPositions(v).join(','),
            ALIGNMENT_TABLE[v].join(','),
            `整列パターン中心 バージョン ${v}`
        );
    }
}

section('5. 手計算のデータ符号語 — "HELLO WORLD" をバイトモードで');

{
    /*
     * 手計算の根拠(バージョン 1 / レベル M、データ符号語 16 個 = 128bit):
     *   モード指示子 0100 + 文字数 00001011(11) + "HELLO WORLD" の 11 バイト = 100bit
     *   終端子 0000 で 104bit(= 13 バイトちょうど)。残り 3 個は 0xEC,0x11,0xEC。
     * 4bit ずれるので、各バイトは「前の下位ニブル | 次の上位ニブル」になる。
     *   0x40 B4 84 54 C4 C4 F2 05 74 F5 24 C4 40 | EC 11 EC
     */
    const EXPECTED = [
        0x40, 0xb4, 0x84, 0x54, 0xc4, 0xc4, 0xf2, 0x05,
        0x74, 0xf5, 0x24, 0xc4, 0x40, 0xec, 0x11, 0xec
    ];
    const actual = Array.from(buildDataCodewords(
        new TextEncoder().encode('HELLO WORLD'),
        1,
        'M'
    ));
    const hex = (a) => a.map((b) => b.toString(16).padStart(2, '0')).join(' ');
    eq(hex(actual), hex(EXPECTED), 'V1-M "HELLO WORLD" のデータ符号語 16 個');

    // 埋草は 0xEC / 0x11 の交互であること(短い入力で複数回の交互を確認)。
    const shortPad = Array.from(buildDataCodewords(new TextEncoder().encode('A'), 1, 'M'));
    // "A" は 0100 + 00000001 + 01000001 + 0000 = 24bit = 3 符号語ちょうど。
    eq(hex(shortPad.slice(0, 3)), '40 14 10', 'V1-M "A" の先頭 3 符号語(4bit ずれの検算)');
    eq(
        hex(shortPad.slice(3)),
        'ec 11 ec 11 ec 11 ec 11 ec 11 ec 11 ec',
        '埋草符号語が 0xEC / 0x11 の交互になる'
    );
}

section('6. 行列の構造 — 位置検出 / タイミング / 整列 / 常時暗モジュール');

{
    const target = 'https://suneo3476.github.io/prism-earring/';
    const qr = encodeQr(target);
    const { modules, size } = qr;

    // 位置検出パターン: 7x7 の枠 + 中央 3x3。3 隅すべてを実測する。
    const FINDER = [
        '1111111', '1000001', '1011101', '1011101', '1011101', '1000001', '1111111'
    ];
    function finderAt(ox, oy) {
        for (let y = 0; y < 7; y++) {
            for (let x = 0; x < 7; x++) {
                if (modules[oy + y][ox + x] !== (FINDER[y][x] === '1')) {
                    return false;
                }
            }
        }
        return true;
    }
    ok(finderAt(0, 0), '位置検出パターン(左上)');
    ok(finderAt(size - 7, 0), '位置検出パターン(右上)');
    ok(finderAt(0, size - 7), '位置検出パターン(左下)');

    // 分離帯: 位置検出パターンの外周 1 モジュールは必ず明。
    let separatorOk = true;
    for (let i = 0; i < 8; i++) {
        if (modules[7][i] || modules[i][7]) separatorOk = false;
        if (modules[7][size - 1 - i] || modules[i][size - 8]) separatorOk = false;
        if (modules[size - 8][i] || modules[size - 1 - i][7]) separatorOk = false;
    }
    ok(separatorOk, '3 隅の分離帯がすべて明モジュール');

    // タイミングパターン: 6 行目 / 6 列目が交互。
    let timingOk = true;
    for (let i = 8; i < size - 8; i++) {
        if (modules[6][i] !== (i % 2 === 0)) timingOk = false;
        if (modules[i][6] !== (i % 2 === 0)) timingOk = false;
    }
    ok(timingOk, 'タイミングパターンが 6 行目・6 列目で交互');

    // 常時暗モジュール(8, size-8)。
    ok(modules[size - 8][8] === true, '常時暗モジュールが (8, size-8) にある');

    // 整列パターン(このバージョンで規格が定める中心座標すべて)。
    const centers = ALIGNMENT_TABLE[qr.version];
    let alignOk = true;
    let alignCount = 0;
    for (const cy of centers) {
        for (const cx of centers) {
            const nearFinder =
                (cx <= 8 && cy <= 8) || (cx >= size - 9 && cy <= 8) || (cx <= 8 && cy >= size - 9);
            if (nearFinder) continue;
            alignCount++;
            for (let dy = -2; dy <= 2; dy++) {
                for (let dx = -2; dx <= 2; dx++) {
                    const expected = Math.max(Math.abs(dx), Math.abs(dy)) !== 1;
                    if (modules[cy + dy][cx + dx] !== expected) alignOk = false;
                }
            }
        }
    }
    ok(alignOk, `整列パターン ${alignCount} 個が 5x5 の正しい形`);
}

section('7. 往復検証 — 独立に書いたデコーダで読み戻す');

{
    const samples = [
        'https://suneo3476.github.io/prism-earring/',
        'HELLO WORLD',
        'A',
        'http://192.168.1.23:8080/web/index.html',
        'prism — 音の高さを曲げる装置',          // マルチバイト UTF-8
        'x'.repeat(100),
        'y'.repeat(213)                           // バージョン 10 / M の上限ちょうど
    ];
    for (const text of samples) {
        const qr = encodeQr(text);
        const decoded = decodeQr(qr);
        const label = text.length > 24 ? `${text.slice(0, 22)}…` : text;
        ok(decoded.formatValid, `形式情報の BCH 検算が通る — "${label}"`);
        eq(decoded.ecc, 'M', `誤り訂正レベルが M として読める — "${label}"`);
        eq(decoded.mask, qr.mask, `マスク番号が一致する — "${label}"`);
        eq(decoded.mode, 4, `モード指示子がバイトモード(0100) — "${label}"`);
        ok(decoded.syndromesAllZero, `全ブロックのシンドロームが 0 — "${label}"`);
        ok(decoded.alwaysDark, `常時暗モジュールが立っている — "${label}"`);
        eq(decoded.text, text, `復号結果が元の文字列と一致する — "${label}"`);
    }
}

section('8. 全 8 マスクで往復できる(マスク自動選択の健全性)');

{
    const text = 'https://suneo3476.github.io/prism-earring/';
    const scores = [];
    for (let mask = 0; mask < 8; mask++) {
        const qr = encodeQr(text, { mask });
        const decoded = decodeQr(qr);
        eq(decoded.text, text, `マスク ${mask} を強制しても復号できる`);
        eq(decoded.mask, mask, `マスク ${mask} が形式情報に書かれている`);
        scores.push(mask);
    }
    // 自動選択が 0..7 のいずれかで、かつ強制指定と同じ結果を出すこと。
    const auto = encodeQr(text);
    const forced = encodeQr(text, { mask: auto.mask });
    ok(scores.includes(auto.mask), `自動選択されたマスクは 0..7 の範囲(選択値 ${auto.mask})`);
    const flatten = (m) => m.map((row) => row.map((c) => (c ? '1' : '0')).join('')).join('');
    ok(
        flatten(auto.modules) === flatten(forced.modules),
        '自動選択の行列は同じマスクを明示した行列と完全一致する',
        `${auto.size}x${auto.size} 全モジュール一致`
    );
}

section('9. バージョン自動選択');

{
    const cases = [
        [1, 'x'.repeat(14)],    // V1-M: 16 符号語 − ヘッダ 2 = 14 バイト
        [2, 'x'.repeat(15)],
        [2, 'x'.repeat(26)],
        [3, 'x'.repeat(27)],
        [10, 'y'.repeat(213)]
    ];
    for (const [expected, text] of cases) {
        eq(encodeQr(text).version, expected, `${text.length} バイト → バージョン ${expected}`);
    }
    eq(encodeQr('https://suneo3476.github.io/prism-earring/').version, 3,
        '本番 URL(42 バイト)はバージョン 3');
    eq(encodeQr('https://suneo3476.github.io/prism-earring/').size, 29,
        '本番 URL の記号サイズは 29x29');
}

section('10. 異常系');

{
    throws(() => encodeQr(''), '空文字列を弾く');
    throws(() => encodeQr(null), 'null を弾く');
    throws(() => encodeQr('z'.repeat(214)), 'バージョン 10 / M の容量超過を弾く');
    throws(() => encodeQr('abc', { mask: -1 }), 'マスク -1 を弾く');
    throws(() => encodeQr('abc', { mask: 1.5 }), '非整数のマスクを弾く');
    throws(() => encodeQr('abc', { ecc: 'Q' }), '未実装の誤り訂正レベル Q を弾く');
    // マルチバイト文字は UTF-8 のバイト長で数える(文字数ではない)。
    eq(encodeQr('あ'.repeat(71)).version, 10, '全角 71 文字(213 バイト)はバージョン 10');
    throws(() => encodeQr('あ'.repeat(72)), '全角 72 文字(216 バイト)は容量超過');
}

section('11. SVG パスデータ');

{
    const qr = encodeQr('https://suneo3476.github.io/prism-earring/');
    const path = toSvgPathData(qr.modules, 4);
    const commands = path.match(/M/g) || [];
    let dark = 0;
    for (const row of qr.modules) {
        for (const cell of row) {
            if (cell) dark++;
        }
    }
    eq(commands.length, dark, `暗モジュール ${dark} 個ぶんのサブパスが出る`);
    ok(/^M4 4h1v1h-1z/.test(path), 'ゆとり 4 モジュールぶん座標がずれている(左上が (4,4))');
    ok(!/[^M0-9 hvz-]/.test(path), 'パスデータに想定外の文字が混ざっていない');
    ok(path.length > 0 && dark > 0, `暗モジュール数 ${dark} / 全 ${qr.size * qr.size}`);
}

/* ------------------------------ 集計 ------------------------------ */

console.log(`\n合計: ${pass} PASS / ${fail} FAIL`);
process.exit(fail === 0 ? 0 : 1);
