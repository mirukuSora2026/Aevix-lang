#!/usr/bin/env node
// ========================
//
// Created By : miruku_sora2026
//
// File name : run_bench.js
//
// Description :
//     공유 명세의 정수 계산을 순차 실행하거나 새 작업 스레드들과 실행한다.
//     Run the shared exact-integer kernels serially or with fresh worker threads.
//
// ========================
"use strict";

const { Worker, isMainThread, workerData } = require("node:worker_threads");
const MODULUS = 2147483647;
const MULTIPLIER = 48271;
const CHECKSUM_MODULUS = 1000000007;

function runRange(values, begin, end, kernel, rounds) {
    if (kernel === "lcg") {
        for (let index = begin; index < end; ++index) {
            let value = values[index] % MODULUS + 1;
            for (let round = 0; round < rounds; ++round) {
                value = (value * MULTIPLIER + 1) % MODULUS;
            }
            values[index] = value;
        }
    } else if (kernel === "next_prime") {
        for (let index = begin; index < end; ++index) {
            let candidate = 1000000001 + (values[index] % 4096) * 1024;
            for (;;) {
                let divisor = 3;
                while (divisor * divisor <= candidate) {
                    if (candidate % divisor === 0) break;
                    divisor += 2;
                }
                if (divisor * divisor > candidate) {
                    values[index] = candidate;
                    break;
                }
                candidate += 2;
            }
        }
    } else {
        throw new Error("Unsupported kernel: " + kernel);
    }
}

function chunks(length, workers) {
    const quotient = Math.floor(length / workers);
    const remainder = length % workers;
    const regions = [];
    let begin = 0;
    for (let index = 0; index < workers; ++index) {
        const end = begin + quotient + (index < remainder ? 1 : 0);
        regions.push([begin, end]);
        begin = end;
    }
    return regions;
}

async function runParallel(values, workers, kernel, rounds) {
    const regions = chunks(values.length, workers);
    const threads = [];
    const completions = [];
    try {
        for (const [begin, end] of regions.slice(0, -1)) {
            const worker = new Worker(__filename, {
                workerData: { buffer: values.buffer, begin, end, kernel, rounds }
            });
            threads.push(worker);
            completions.push(new Promise((resolve, reject) => {
                let failure = null;
                worker.once("error", error => { failure = error; });
                worker.once("exit", code => {
                    if (failure) reject(failure);
                    else if (code === 0) resolve();
                    else reject(new Error("Benchmark worker exited with code " + code));
                });
            }));
        }
        const [begin, end] = regions[regions.length - 1];
        runRange(values, begin, end, kernel, rounds);
    } catch (error) {
        await Promise.allSettled(threads.map(worker => worker.terminate()));
        await Promise.allSettled(completions);
        throw error;
    }
    // 각 작업자의 종료까지 기다려 데이터 대여와 측정 구간을 함께 끝낸다.
    // Await every worker exit before ending the data borrow and measured interval.
    const outcomes = await Promise.allSettled(completions);
    const failed = outcomes.find(outcome => outcome.status === "rejected");
    if (failed) throw failed.reason;
}

function loadSpec() {
    const fs = require("node:fs");
    const path = require("node:path");
    const spec = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "spec.json"), "utf8"));
    if (spec.schema_version !== 1 || spec.elements !== 4096) {
        throw new Error("This runner requires cpu4096 spec schema 1 with 4096 elements");
    }
    if (spec.lcg !== "x=values[i]%2147483647+1; repeat rounds times: x=(x*48271+1)%2147483647; store x") {
        throw new Error("The LCG contract changed; update the runner explicitly");
    }
    if (spec.next_prime !== "n=1000000001+(values[i]%4096)*1024; test odd divisors d=3,5,... while d*d<=n; if composite n+=2 and retry; store the first prime") {
        throw new Error("The prime-search contract changed; update the runner explicitly");
    }
    if (JSON.stringify(spec.modes) !== JSON.stringify(["serial", "parallel_fresh"])) {
        throw new Error("The benchmark mode contract changed");
    }
    if (!spec.input.startsWith("values[i]=(i+seed)%4096")) {
        throw new Error("The runtime-seeded input contract changed");
    }
    return spec;
}

function parseCount(value, name, minimum) {
    if (!/^[0-9]+$/.test(value)) throw new Error(name + " must be a decimal integer");
    const count = Number(value);
    if (!Number.isSafeInteger(count) || count < minimum) {
        throw new Error(name + " must be a safe integer >= " + minimum);
    }
    return count;
}

function seedReader(fallback, length) {
    if (process.stdin.isTTY) return { next: async () => fallback, close: () => {} };
    const reader = require("node:readline").createInterface({ input: process.stdin, crlfDelay: Infinity });
    const lines = reader[Symbol.asyncIterator]();
    return {
        next: async () => {
            const line = await lines.next();
            return line.done ? fallback : parseCount(line.value.trim(), "stdin seed", 0) % length;
        },
        close: () => { reader.close(); process.stdin.pause(); }
    };
}

async function main(args) {
    const spec = loadSpec();
    if (args.length < 2 || args.length > 5) {
        throw new Error("Usage: node run_bench.js CASE MODE [REPEATS=5] [WARMUPS=1] [SEED=0]");
    }
    const [caseName, mode] = args;
    if (!Object.hasOwn(spec.cases, caseName) || !spec.modes.includes(mode)) {
        throw new Error("Unknown case or mode; see ../spec.json");
    }
    const repeats = args.length > 2 ? parseCount(args[2], "repeats", 1) : spec.measurement.samples;
    const warmups = args.length > 3 ? parseCount(args[3], "warmups", 0) : spec.measurement.warmups;
    const fallbackSeed = (args.length > 4 ? parseCount(args[4], "seed", 0) : 0) % spec.elements;
    const { kernel, rounds = 0 } = spec.cases[caseName];
    if (!["lcg", "next_prime"].includes(kernel) || (kernel === "lcg" && (!Number.isSafeInteger(rounds) || rounds < 1))) {
        throw new Error("Unsupported case configuration");
    }
    const logical = Math.max(1, require("node:os").cpus().length);
    const workers = mode === "parallel_fresh" ? Math.min(logical, 32, spec.elements) : 1;
    const values = new Float64Array(new SharedArrayBuffer(spec.elements * Float64Array.BYTES_PER_ELEMENT));
    const seeds = seedReader(fallbackSeed, values.length);
    try {
        for (let iteration = 0; iteration < warmups + repeats; ++iteration) {
            // 입력은 부모만 회차별로 소비하며 타이머를 시작하기 전에 정규화한다.
            // Only the parent consumes each seed and normalizes it before starting timing.
            const seed = await seeds.next();
            // 공유 버퍼 초기화와 체크섬·출력은 타이머 밖에 둔다.
            // Keep shared-buffer initialization, checksums, and output outside timing.
            for (let index = 0; index < values.length; ++index) values[index] = (index + seed) % values.length;
            const start = process.hrtime.bigint();
            if (mode === "parallel_fresh") await runParallel(values, workers, kernel, rounds);
            else runRange(values, 0, values.length, kernel, rounds);
            const elapsed = process.hrtime.bigint() - start;
            if (iteration >= warmups) {
                let total = 0;
                let weighted = 0;
                for (let index = 0; index < values.length; ++index) {
                    total += values[index];
                    weighted = (weighted + (index + 1) * values[index]) % CHECKSUM_MODULUS;
                }
                if (!Number.isSafeInteger(total) || !Number.isSafeInteger(weighted)) {
                    throw new Error("Checksum exceeded the shared exact-integer domain");
                }
                console.log("SAMPLE", caseName, mode, iteration - warmups, elapsed.toString(), total, weighted, workers);
            }
        }
        console.log("VERIFY", caseName, mode, Array.from(values).join(","));
    } finally {
        seeds.close();
    }
}

if (isMainThread && require.main === module) {
    main(process.argv.slice(2)).catch(error => {
        console.error("Benchmark error:", error.message);
        process.exitCode = 1;
    });
} else if (!isMainThread) {
    runRange(new Float64Array(workerData.buffer), workerData.begin, workerData.end, workerData.kernel, workerData.rounds);
}

module.exports = { runRange, chunks, loadSpec };
