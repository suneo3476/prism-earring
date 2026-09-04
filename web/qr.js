/*
 * qr.js — QR コードエンコーダ(バイトモード / 誤り訂正レベル M / バージョン 1〜10)
 *
 * 外部依存ゼロ。DOM にも触れない純関数の集まりなので、ブラウザからも
 * Node(web/test/qr-test.mjs)からも同じコードを検証できる。
 * 規格: ISO/IEC 18004。マスクは 8 通りすべてを試して減点法で自動選択する。
 *
 * 使う側は encodeQr(text) と toSvgPathData(modules, border) だけ見ればよい。
 */

/* ------------------------- GF(256) 演算 ------------------------- */

/* 原始多項式 x^8 + x^4 + x^3 + x^2 + 1 = 0x11D(QR 規格が定めるもの)。 */
const GF_PRIMITIVE = 0x11d;

const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);

(function initGaloisTables() {
    let x = 1;
    for (let i = 0; i < 255; i++) {
        GF_EXP[i] = x;
        GF_LOG[x] = i;
        x <<= 1;
        if (x & 0x100) {
            x ^= GF_PRIMITIVE;
        }
    }
    // 添字 255..509 を折り返しておくと、乗算で剰余を取らずに済む。
    for (let i = 255; i < 512; i++) {
        GF_EXP[i] = GF_EXP[i - 255];
    }
})();

/** GF(256) の乗算。0 は対数を持たないので先に弾く。 */
function gfMul(a, b) {
    if (a === 0 || b === 0) {
        return 0;
    }
    return GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

/** GF(256) 上の α^k。テスト側の独立検算にも使う。 */
function gfPow(k) {
    return GF_EXP[((k % 255) + 255) % 255];
}

/* --------------------- Reed–Solomon 符号化 --------------------- */

/**
 * 生成多項式 g(x) = (x - α^0)(x - α^1) ... (x - α^(degree-1)) の係数を
 * 降べき順で返す。先頭の x^degree の係数 1 は自明なので含めない。
 */
function rsDivisor(degree) {
    if (degree < 1 || degree > 255) {
        throw new RangeError(`rsDivisor: degree が範囲外です(${degree})`);
    }
    const result = new Uint8Array(degree);
    result[degree - 1] = 1;             // 多項式「1」から始める
    let root = 1;                       // α^0
    for (let i = 0; i < degree; i++) {
        // result(x) に (x - α^i) を掛ける。GF(2^n) なので減算は加算(XOR)と同じ。
        for (let j = 0; j < degree; j++) {
            result[j] = gfMul(result[j], root);
            if (j + 1 < degree) {
                result[j] ^= result[j + 1];
            }
        }
        root = gfMul(root, 2);          // α^(i+1)
    }
    return result;
}

/** data(x) * x^degree を divisor で割った剰余 = 誤り訂正コードワード。 */
function rsRemainder(data, divisor) {
    const result = new Uint8Array(divisor.length);
    for (const byte of data) {
        const factor = byte ^ result[0];
        result.copyWithin(0, 1);
        result[result.length - 1] = 0;
        for (let i = 0; i < result.length; i++) {
            result[i] ^= gfMul(divisor[i], factor);
        }
    }
    return result;
}

/* -------------------------- 版・容量表 -------------------------- */

const MIN_VERSION = 1;
const MAX_VERSION = 10;

/** 形式情報に載る 2 bit。規格の並びは L=01 / M=00 / Q=11 / H=10 で数値順ではない。 */
const ECC_FORMAT_BITS = { L: 1, M: 0, Q: 3, H: 2 };

/**
 * [ブロックあたりの誤り訂正コードワード数, ブロック数] をバージョン 1..10 ぶん。
 * M が本番用。L は形式情報の既知ベクタ照合と将来の拡張のために持っておく。
 */
const ECC_TABLE = {
    L: [[7, 1], [10, 1], [15, 1], [20, 1], [26, 1], [18, 2], [20, 2], [24, 2], [30, 2], [18, 4]],
    M: [[10, 1], [16, 1], [26, 1], [18, 2], [24, 2], [16, 4], [18, 4], [22, 4], [22, 5], [26, 5]]
};

/** そのバージョンで符号語に使えるモジュール数 / 8 = 総コードワード数。 */
function totalCodewords(version) {
    let modules = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const numAlign = Math.floor(version / 7) + 2;
        modules -= (25 * numAlign - 10) * numAlign - 55;
        if (version >= 7) {
            modules -= 36;                  // バージョン情報 18bit × 2
        }
    }
    return Math.floor(modules / 8);
}

function eccParams(version, ecc) {
    const table = ECC_TABLE[ecc];
    if (!table) {
        throw new RangeError(`未対応の誤り訂正レベルです: ${ecc}`);
    }
    const [eccPerBlock, numBlocks] = table[version - MIN_VERSION];
    return { eccPerBlock, numBlocks };
}

/** データコードワード数(= 総数 − 誤り訂正ぶん)。 */
function dataCodewords(version, ecc) {
    const { eccPerBlock, numBlocks } = eccParams(version, ecc);
    return totalCodewords(version) - eccPerBlock * numBlocks;
}

/** バイトモードの文字数指示子のビット幅。バージョン 10 以上は 16bit。 */
function charCountBits(version) {
    return version <= 9 ? 8 : 16;
}

/** 整列パターンの中心座標。バージョン 1 は無し。 */
function alignmentPositions(version) {
    if (version === 1) {
        return [];
    }
    const numAlign = Math.floor(version / 7) + 2;
    const step = Math.floor((version * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2)) * 2;
    const result = [];
    for (let i = 0, pos = version * 4 + 10; i < numAlign - 1; i++, pos -= step) {
        result.unshift(pos);
    }
    result.unshift(6);
    return result;
}

/* ------------------------- 形式・版情報 ------------------------- */

/**
 * 形式情報 15bit。5bit(誤り訂正 2 + マスク 3)に BCH(15,5) の
 * 10bit を付け、規格のマスク 0x5412 と XOR する。
 */
function formatBits(ecc, mask) {
    const eccBits = ECC_FORMAT_BITS[ecc];
    if (eccBits === undefined) {
        throw new RangeError(`未対応の誤り訂正レベルです: ${ecc}`);
    }
    if (!Number.isInteger(mask) || mask < 0 || mask > 7) {
        throw new RangeError(`マスク番号が範囲外です: ${mask}`);
    }
    const data = (eccBits << 3) | mask;
    let rem = data;
    for (let i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    }
    return ((data << 10) | (rem & 0x3ff)) ^ 0x5412;
}

/** バージョン情報 18bit(バージョン 7 以上のみ使う)。BCH(18,6)。 */
function versionBits(version) {
    let rem = version;
    for (let i = 0; i < 12; i++) {
        rem = (rem << 1) ^ ((rem >>> 11) * 0x1f25);
    }
    return (version << 12) | (rem & 0xfff);
}

/* --------------------------- ビット列 --------------------------- */

function utf8Bytes(text) {
    if (typeof TextEncoder === 'function') {
        return new TextEncoder().encode(text);
    }
    // TextEncoder が無い環境向けの手書き UTF-8(古い WebView 等)。
    const out = [];
    for (const ch of String(text)) {
        let cp = ch.codePointAt(0);
        if (cp < 0x80) {
            out.push(cp);
        } else if (cp < 0x800) {
            out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
        } else {
            out.push(
                0xf0 | (cp >> 18),
                0x80 | ((cp >> 12) & 0x3f),
                0x80 | ((cp >> 6) & 0x3f),
                0x80 | (cp & 0x3f)
            );
        }
    }
    return Uint8Array.from(out);
}

function appendBits(bits, value, width) {
    for (let i = width - 1; i >= 0; i--) {
        bits.push((value >>> i) & 1);
    }
}

/** 収まる最小のバージョンを選ぶ。10 でも収まらなければ投げる。 */
function pickVersion(byteLength, ecc) {
    for (let version = MIN_VERSION; version <= MAX_VERSION; version++) {
        const capacityBits = dataCodewords(version, ecc) * 8;
        const neededBits = 4 + charCountBits(version) + byteLength * 8;
        if (neededBits <= capacityBits) {
            return version;
        }
    }
    throw new RangeError(
        `データが長すぎます(${byteLength} バイト)。バージョン ${MAX_VERSION} / レベル ${ecc} の上限は ` +
        `${dataCodewords(MAX_VERSION, ecc) - 3} バイトです。`
    );
}

/** バイトモード 1 セグメントぶんのデータコードワードを作る(終端・埋草込み)。 */
function buildDataCodewords(bytes, version, ecc) {
    const capacityBits = dataCodewords(version, ecc) * 8;
    const bits = [];
    appendBits(bits, 0b0100, 4);                            // バイトモード指示子
    appendBits(bits, bytes.length, charCountBits(version)); // 文字数指示子
    for (const byte of bytes) {
        appendBits(bits, byte, 8);
    }
    // 終端子は最大 4bit。容量ぎりぎりなら短くて構わない。
    for (let i = 0; i < 4 && bits.length < capacityBits; i++) {
        bits.push(0);
    }
    // バイト境界まで 0 で詰める。
    while (bits.length % 8 !== 0) {
        bits.push(0);
    }
    const result = new Uint8Array(capacityBits / 8);
    for (let i = 0; i < bits.length; i += 8) {
        let byte = 0;
        for (let j = 0; j < 8; j++) {
            byte = (byte << 1) | bits[i + j];
        }
        result[i / 8] = byte;
    }
    // 残りは 0xEC / 0x11 の交互(規格が定める埋草コードワード)。
    for (let i = bits.length / 8, pad = 0xec; i < result.length; i++, pad ^= 0xec ^ 0x11) {
        result[i] = pad;
    }
    return result;
}

/** ブロック分割 → 各ブロックに ECC を付与 → 規格どおりインターリーブ。 */
function addEccAndInterleave(data, version, ecc) {
    const { eccPerBlock, numBlocks } = eccParams(version, ecc);
    const rawCodewords = totalCodewords(version);
    if (data.length !== rawCodewords - eccPerBlock * numBlocks) {
        throw new RangeError('addEccAndInterleave: データ長がバージョンと一致しません');
    }
    const numShortBlocks = numBlocks - (rawCodewords % numBlocks);
    const shortBlockLen = Math.floor(rawCodewords / numBlocks);

    const divisor = rsDivisor(eccPerBlock);
    const blocks = [];
    for (let i = 0, k = 0; i < numBlocks; i++) {
        const dataLen = shortBlockLen - eccPerBlock + (i < numShortBlocks ? 0 : 1);
        const dat = data.subarray(k, k + dataLen);
        k += dataLen;
        blocks.push({ dat, ecc: rsRemainder(dat, divisor) });
    }

    const result = new Uint8Array(rawCodewords);
    let k = 0;
    // データ部を列方向に読み出す。短いブロックには存在しない位置を飛ばす。
    for (let i = 0; i < shortBlockLen - eccPerBlock + 1; i++) {
        for (let j = 0; j < numBlocks; j++) {
            if (i < blocks[j].dat.length) {
                result[k++] = blocks[j].dat[i];
            }
        }
    }
    // 誤り訂正部は全ブロック同じ長さなのでそのまま列方向に読み出す。
    for (let i = 0; i < eccPerBlock; i++) {
        for (let j = 0; j < numBlocks; j++) {
            result[k++] = blocks[j].ecc[i];
        }
    }
    if (k !== rawCodewords) {
        throw new Error('addEccAndInterleave: インターリーブ長が合いません');
    }
    return result;
}

/* --------------------------- 行列の構築 --------------------------- */

function makeGrid(size, value) {
    const grid = [];
    for (let y = 0; y < size; y++) {
        grid.push(new Array(size).fill(value));
    }
    return grid;
}

/** 機能パターン(位置検出・分離・タイミング・整列・暗モジュール)を描く。 */
function drawFunctionPatterns(modules, isFunction, size, version) {
    function set(x, y, dark) {
        if (x < 0 || y < 0 || x >= size || y >= size) {
            return;
        }
        modules[y][x] = dark;
        isFunction[y][x] = true;
    }

    // タイミングパターン(6 行目 / 6 列目)。
    for (let i = 0; i < size; i++) {
        set(6, i, i % 2 === 0);
        set(i, 6, i % 2 === 0);
    }

    // 位置検出パターン 3 個。中心から 5x5 を塗り、周囲 1 モジュールを分離帯にする。
    for (const [cx, cy] of [[3, 3], [size - 4, 3], [3, size - 4]]) {
        for (let dy = -4; dy <= 4; dy++) {
            for (let dx = -4; dx <= 4; dx++) {
                const dist = Math.max(Math.abs(dx), Math.abs(dy));
                set(cx + dx, cy + dy, dist !== 2 && dist !== 4);
            }
        }
    }

    // 整列パターン。3 隅の位置検出パターンと重なる組み合わせだけ除く。
    const positions = alignmentPositions(version);
    const n = positions.length;
    for (let i = 0; i < n; i++) {
        for (let j = 0; j < n; j++) {
            const corner =
                (i === 0 && j === 0) || (i === 0 && j === n - 1) || (i === n - 1 && j === 0);
            if (corner) {
                continue;
            }
            for (let dy = -2; dy <= 2; dy++) {
                for (let dx = -2; dx <= 2; dx++) {
                    set(
                        positions[i] + dx,
                        positions[j] + dy,
                        Math.max(Math.abs(dx), Math.abs(dy)) !== 1
                    );
                }
            }
        }
    }

    // 形式情報の領域を先に予約する(値はマスク確定後に上書きする)。
    drawFormatBits(modules, isFunction, size, 'M', 0, true);

    // バージョン情報(7 以上)。
    if (version >= 7) {
        const bits = versionBits(version);
        for (let i = 0; i < 18; i++) {
            const dark = ((bits >>> i) & 1) !== 0;
            const a = size - 11 + (i % 3);
            const b = Math.floor(i / 3);
            set(a, b, dark);
            set(b, a, dark);
        }
    }
}

/** 形式情報 15bit を 2 か所へ書く。reserve=true のときは機能領域として印を付ける。 */
function drawFormatBits(modules, isFunction, size, ecc, mask, reserve) {
    const bits = formatBits(ecc, mask);
    function set(x, y, dark) {
        modules[y][x] = dark;
        if (reserve) {
            isFunction[y][x] = true;
        }
    }
    function bit(i) {
        return ((bits >>> i) & 1) !== 0;
    }

    // 1 つ目: 左上の位置検出パターンを囲む L 字。
    for (let i = 0; i <= 5; i++) {
        set(8, i, bit(i));
    }
    set(8, 7, bit(6));
    set(8, 8, bit(7));
    set(7, 8, bit(8));
    for (let i = 9; i < 15; i++) {
        set(14 - i, 8, bit(i));
    }

    // 2 つ目: 右上と左下に分割して置く。
    for (let i = 0; i < 8; i++) {
        set(size - 1 - i, 8, bit(i));
    }
    for (let i = 8; i < 15; i++) {
        set(8, size - 15 + i, bit(i));
    }
    set(8, size - 8, true);     // 常に暗いモジュール
}

/** ジグザグ走査でコードワードのビットを機能領域以外へ流し込む。 */
function drawCodewords(modules, isFunction, size, codewords) {
    let i = 0;
    for (let right = size - 1; right >= 1; right -= 2) {
        if (right === 6) {
            right = 5;              // 6 列目はタイミングパターンなので飛ばす
        }
        for (let vert = 0; vert < size; vert++) {
            for (let j = 0; j < 2; j++) {
                const x = right - j;
                const upward = ((right + 1) & 2) === 0;
                const y = upward ? size - 1 - vert : vert;
                if (!isFunction[y][x] && i < codewords.length * 8) {
                    modules[y][x] = ((codewords[i >>> 3] >>> (7 - (i & 7))) & 1) !== 0;
                    i++;
                }
            }
        }
    }
    if (i !== codewords.length * 8) {
        throw new Error(`drawCodewords: 配置したビット数が合いません(${i})`);
    }
}

const MASK_RULES = [
    (x, y) => (x + y) % 2 === 0,
    (x, y) => y % 2 === 0,
    (x) => x % 3 === 0,
    (x, y) => (x + y) % 3 === 0,
    (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0,
    (x, y) => ((x * y) % 2) + ((x * y) % 3) === 0,
    (x, y) => (((x * y) % 2) + ((x * y) % 3)) % 2 === 0,
    (x, y) => (((x + y) % 2) + ((x * y) % 3)) % 2 === 0
];

/** マスクを XOR で適用する。同じ引数でもう一度呼べば元に戻る。 */
function applyMask(modules, isFunction, size, mask) {
    const rule = MASK_RULES[mask];
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            if (!isFunction[y][x] && rule(x, y)) {
                modules[y][x] = !modules[y][x];
            }
        }
    }
}

/* 減点法の重み(規格の N1..N4)。 */
const PENALTY_N1 = 3;
const PENALTY_N2 = 3;
const PENALTY_N3 = 40;
const PENALTY_N4 = 10;

/* 規則 3 が探す 1:1:3:1:1 比のパターンと、それに続く/先立つ 4 モジュールの明部。 */
const FINDER_LIKE_A = [true, false, true, true, true, false, true, false, false, false, false];
const FINDER_LIKE_B = [false, false, false, false, true, false, true, true, true, false, true];

function matchesAt(line, start, pattern) {
    for (let i = 0; i < pattern.length; i++) {
        if (line[start + i] !== pattern[i]) {
            return false;
        }
    }
    return true;
}

function penaltyScore(modules, size) {
    let score = 0;

    // 規則 1: 同色 5 連以上。5 個で 3 点、以降 1 個ごとに 1 点。
    // 規則 3: 位置検出パターンに似た並び 1 個につき 40 点。
    for (let pass = 0; pass < 2; pass++) {
        for (let a = 0; a < size; a++) {
            const line = new Array(size);
            for (let b = 0; b < size; b++) {
                line[b] = pass === 0 ? modules[a][b] : modules[b][a];
            }
            let runLength = 1;
            for (let b = 1; b < size; b++) {
                if (line[b] === line[b - 1]) {
                    runLength++;
                } else {
                    if (runLength >= 5) {
                        score += PENALTY_N1 + (runLength - 5);
                    }
                    runLength = 1;
                }
            }
            if (runLength >= 5) {
                score += PENALTY_N1 + (runLength - 5);
            }
            for (let b = 0; b + 11 <= size; b++) {
                if (matchesAt(line, b, FINDER_LIKE_A) || matchesAt(line, b, FINDER_LIKE_B)) {
                    score += PENALTY_N3;
                }
            }
        }
    }

    // 規則 2: 同色 2x2 ブロック 1 個につき 3 点。
    for (let y = 0; y + 1 < size; y++) {
        for (let x = 0; x + 1 < size; x++) {
            const c = modules[y][x];
            if (c === modules[y][x + 1] && c === modules[y + 1][x] && c === modules[y + 1][x + 1]) {
                score += PENALTY_N2;
            }
        }
    }

    // 規則 4: 暗モジュール比率の 50% からの隔たり。5% ごとに 10 点。
    let dark = 0;
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            if (modules[y][x]) {
                dark++;
            }
        }
    }
    const total = size * size;
    // size は奇数なので dark/total がちょうど 1/2 になることはなく、k >= 0 が保証される。
    const k = Math.ceil(Math.abs(dark * 20 - total * 10) / total) - 1;
    score += k * PENALTY_N4;

    return score;
}

/* ----------------------------- 入口 ----------------------------- */

/**
 * text を QR コードへ符号化する。
 * @param {string} text 符号化する文字列(UTF-8 バイトモード)
 * @param {{ecc?: 'L'|'M', mask?: number}} [options] mask を渡すと自動選択せずそれを使う(検証用)
 * @returns {{version: number, size: number, mask: number, ecc: string, modules: boolean[][]}}
 */
function encodeQr(text, options = {}) {
    const ecc = options.ecc || 'M';
    if (typeof text !== 'string' || text.length === 0) {
        throw new RangeError('encodeQr: 空でない文字列が必要です');
    }
    const bytes = utf8Bytes(text);
    const version = pickVersion(bytes.length, ecc);
    const size = version * 4 + 17;

    const data = buildDataCodewords(bytes, version, ecc);
    const codewords = addEccAndInterleave(data, version, ecc);

    const modules = makeGrid(size, false);
    const isFunction = makeGrid(size, false);
    drawFunctionPatterns(modules, isFunction, size, version);
    drawCodewords(modules, isFunction, size, codewords);

    // マスクの決定。指定があればそれ、無ければ 8 通りの減点を比べて最小を採る。
    let bestMask = options.mask;
    if (bestMask === undefined) {
        let bestScore = Infinity;
        for (let mask = 0; mask < 8; mask++) {
            applyMask(modules, isFunction, size, mask);
            drawFormatBits(modules, isFunction, size, ecc, mask, false);
            const score = penaltyScore(modules, size);
            if (score < bestScore) {
                bestScore = score;
                bestMask = mask;
            }
            applyMask(modules, isFunction, size, mask);     // XOR なので戻る
        }
    } else if (!Number.isInteger(bestMask) || bestMask < 0 || bestMask > 7) {
        throw new RangeError(`マスク番号が範囲外です: ${bestMask}`);
    }

    applyMask(modules, isFunction, size, bestMask);
    drawFormatBits(modules, isFunction, size, ecc, bestMask, false);

    return { version, size, mask: bestMask, ecc, modules };
}

/**
 * 暗モジュールを 1 本の SVG パスにまとめる。左上を (border, border) とする
 * 「1 モジュール = 1 単位」の座標系で出力するので、viewBox は size + border*2。
 */
function toSvgPathData(modules, border = 4) {
    const size = modules.length;
    const parts = [];
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            if (modules[y][x]) {
                parts.push(`M${x + border} ${y + border}h1v1h-1z`);
            }
        }
    }
    return parts.join('');
}

export {
    encodeQr,
    toSvgPathData,
    // 以下は web/test/qr-test.mjs からの検証用。本番コードは使わない。
    rsDivisor,
    rsRemainder,
    formatBits,
    versionBits,
    totalCodewords,
    dataCodewords,
    eccParams,
    alignmentPositions,
    buildDataCodewords,
    addEccAndInterleave,
    penaltyScore,
    gfMul,
    gfPow,
    utf8Bytes,
    MASK_RULES,
    MAX_VERSION
};
