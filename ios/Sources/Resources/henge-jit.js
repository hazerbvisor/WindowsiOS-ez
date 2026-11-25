const CMD_DETACH = 0;
const CMD_PREPARE_REGION = 1;
const CMD_NEW_BREAKPOINTS = 2;
const commands = {
    [CMD_DETACH]: JIT26Detach,
    [CMD_PREPARE_REGION]: JIT26PrepareRegion,
    [CMD_NEW_BREAKPOINTS]: JIT26NewBreakpoints
};
const legacyCommands = {
    [0x68]: JIT26NewBreakpoints,
    [0x69]: JIT26HandleBrk0x69,
    [0xf00d]: JIT26HandleBrk0xf00d
};

let tid, x0, x1, x16, pc;

let detached = false;
let pid = get_pid();
log(`pid = ${pid}`);
let attachResponse = send_command(`vAttach;${pid.toString(16)}`);
log(`attach_response = ${attachResponse}`);
    
let totalBreakpoints = 0;
while (!detached) {
    totalBreakpoints++;
    log(`Handling breakpoint ${totalBreakpoints}`);
    
    let brkResponse = send_command(`c`);
    log(`brkResponse = ${brkResponse}`);
    
    let tmpMatch = /T[0-9a-f]+thread:(?<tid>[0-9a-f]+);/.exec(brkResponse);
    tid = tmpMatch ? tmpMatch.groups['tid'] : null;
    tmpMatch = /20:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    pc = tmpMatch ? tmpMatch.groups['reg'] : null;
    tmpMatch = /10:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
    x16 = tmpMatch ? tmpMatch.groups['reg'] : null;
    if (!tid || !pc || !x16) {
        log(`Failed to extract registers: tid=${tid}, pc=${pc}, x16=${x16}`);
        continue;
    }
    pc = littleEndianHexStringToNumber(pc);
    x16 = littleEndianHexStringToNumber(x16);
    
    let instructionResponse = send_command(`m${pc.toString(16)},4`);
    log(`instruction at pc: ${instructionResponse}`);
    let instrU32 = littleEndianHexToU32(instructionResponse);
    let brkImmediate = extractBrkImmediate(instrU32);
    log(`BRK immediate: 0x${brkImmediate.toString(16)} (${brkImmediate})`);
    if (legacyCommands[brkImmediate] != undefined) {
        // when we find a valid brk immediate command, parse x0 and x1
        tmpMatch = /00:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
        x0 = tmpMatch ? tmpMatch.groups['reg'] : null;
        tmpMatch = /01:(?<reg>[0-9a-f]{16});/.exec(brkResponse);
        x1 = tmpMatch ? tmpMatch.groups['reg'] : null;
        if (!x0 || !x1) {
            log(`Failed to extract registers: x0=${x0}, x1=${x1}`);
            continue;
        }
        x0 = littleEndianHexStringToNumber(x0);
        x1 = littleEndianHexStringToNumber(x1);
        
        // jump over brk
        let pcPlus4 = numberToLittleEndianHexString(pc + 4n);
        let pcPlus4Response = send_command(`P20=${pcPlus4};thread:${tid};`);
        log(`pcPlus4Response = ${pcPlus4Response}`);
        
        // dispatch brk-immediate command
        const command = legacyCommands[brkImmediate];
        command(brkResponse);
    } else {
        log(`Skipping breakpoint: brk immediate 0x${brkImmediate.toString(16)} was not handled by this script. You could add it by evaluating legacyCommands[0x${brkImmediate.toString(16)}] = yourFunction;`);
        continue;
    }
}

function JIT26Detach() {
    let detachResponse = send_command(`D`);
    log(`detachResponse = ${detachResponse}`);
    detached = true;
}

// brk 0x68
function JIT26NewBreakpoints(brkResponse) {
    let instructionResponse = send_command(`m${pc.toString(16)},4`);
    log(`instruction at pc: ${instructionResponse}`);
    let instrU32 = littleEndianHexToU32(instructionResponse);
    let brkImmediate = extractBrkImmediate(instrU32);
    
    let memResponse = send_command(`m${x0.toString(16)},${x1}`);

    let scriptText = hexToAscii(memResponse);
    log(`Script text: ${scriptText}`);

    const res = runScriptAndCapture(scriptText);
    if (res.ok) {
        log('Script succeeded:', res.value);
    } else {
        log('Script failed:', res.name, res.message);
        log(res.stack);
    }
}

// brk 0x69
function JIT26HandleBrk0x69(brkResponse) {
    let putX0Response = send_command(`P0=E0000069;thread:${tid};`);
    log(`putX0Response = ${putX0Response}`);
}

// brk 0xf00d
function JIT26HandleBrk0xf00d(brkResponse) {
    // dispatch command via x16
    const command = commands[x16];
    if (command === undefined) {
        log(`Unknown command ${x16.toString(16)}`);
        return;
    }
    log(`Invoking command ${x16.toString(16)}`);
    command(brkResponse);
}

function JIT26PrepareRegion() {
    const lowLimit = 0x7000000000n;
    const maxBytes = 1536n * 1024n * 1024n;
    let address = x0;
    let fresh = false;
    // Values 1–3 are failure codes. Valid executable addresses are page aligned.
    let result = 1n;
    try {
        if (x1 < 0x4000n || x1 > maxBytes || (x1 & 0x3fffn) !== 0n) {
            throw new Error(`Invalid JIT allocation size: ${x1}`);
        }
        if (address === 0n) {
            const response = send_command(`_M${x1.toString(16)},rx`);
            if (typeof response !== 'string' || /^E[0-9a-f]{2}(?:$|;)/i.test(response) ||
                !/^[0-9a-f]+$/i.test(response)) {
                throw new Error(`Debugger RX allocation failed: ${response}`);
            }
            address = BigInt(`0x${response}`);
            fresh = address !== 0n;
        }
        if (address === 0n || (address & 0x3fffn) !== 0n || address + x1 > lowLimit) {
            result = 3n;
            throw new Error(`RX range 0x${address.toString(16)} + ${x1} is outside the supported TXM window`);
        }
        result = 2n;
        const prepared = prepare_memory_region(address, x1);
        if (prepared === false || (typeof prepared === 'string' && /^E[0-9a-f]{2}(?:$|;)/i.test(prepared))) {
            throw new Error(`Debugger could not prepare RX pages: ${prepared}`);
        }
        log(`Prepared ${x1} RX bytes at 0x${address.toString(16)}`);
        result = address;
    } catch (error) {
        log(error.message);
        if (fresh) {
            try {
                log(`RX deallocation: ${send_command(`_m${address.toString(16)}`)}`);
            } catch (deallocationError) {
                log(deallocationError.message);
            }
        }
    }
    log(`RX register response: ${send_command(`P0=${numberToLittleEndianHexString(result)};thread:${tid};`)}`);
}

// utilities
function littleEndianHexStringToNumber(hexStr) {
    const bytes = [];
    for (let i = 0; i < hexStr.length; i += 2) {
        bytes.push(parseInt(hexStr.substr(i, 2), 16));
    }
    let num = 0n;
    for (let i = bytes.length - 1; i >= 0; i--) {
        num = (num << 8n) | BigInt(bytes[i]);
    }
    return num;
}

function numberToLittleEndianHexString(num) {
    const bytes = [];
    for (let i = 0; i < 8; i++) {
        bytes.push(Number(num & 0xFFn));
        num >>= 8n;
    }
    return bytes.map(b => b.toString(16).padStart(2, '0')).join('');
}

function littleEndianHexToU32(hexStr) {
    return parseInt(hexStr.match(/../g).reverse().join(''), 16);
}

function extractBrkImmediate(u32) {
    return (u32 >> 5) & 0xFFFF;
}

function hexToAscii(hexStr) {
    let str = '';
    for (let i = 0; i < hexStr.length; i += 2) {
        const byte = parseInt(hexStr.substr(i, 2), 16);
        if (byte === 0) break; 
        str += String.fromCharCode(byte);
    }
    return str;
}

function runScriptAndCapture(scriptText) {
    try {
        const value = eval(scriptText);
        return { ok: true, value };
    } catch (err) {
        return {
            ok: false,
            name: err && err.name,
            message: err && err.message,
            stack: err && err.stack
        };
    }
}