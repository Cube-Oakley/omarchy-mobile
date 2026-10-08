.pragma library
// The calculator's arithmetic. Expressions are text as the keys type it
// (× ÷ − for the operators, π and e, sin( cos( tan( ln( log( √( for the
// functions, ! and % after a number, ^ for powers, and 1.5E20 for a number
// in scientific notation), evaluated by a recursive-descent parser, never
// eval(). Precedence, loosest first: + −; × ÷ and juxtaposition (2π, 3(4));
// a sign; ^ (right to left, so 2^3^2 is 2^9 and −2^2 is −4); ! and %.
// Open parentheses close at the end, so "2×(3+4" is 14.

var FUNCTIONS = ["sin", "cos", "tan", "ln", "log", "√"];

function tokenize(text) {
    var tokens = [];
    var i = 0;
    while (i < text.length) {
        var c = text[i];
        if (c === " ") { i++; continue; }
        var number = /^(\d+\.?\d*|\.\d+)(E[+−-]?\d+)?/.exec(text.slice(i));
        if (number) {
            tokens.push({kind: "number", value: parseFloat(number[0].replace("−", "-"))});
            i += number[0].length;
            continue;
        }
        var name = null;
        for (var f = 0; f < FUNCTIONS.length; f++)
            if (text.substr(i, FUNCTIONS[f].length) === FUNCTIONS[f]) { name = FUNCTIONS[f]; break; }
        if (name) { tokens.push({kind: "function", value: name}); i += name.length; continue; }
        if (c === "π") { tokens.push({kind: "number", value: Math.PI}); i++; continue; }
        if (c === "e") { tokens.push({kind: "number", value: Math.E}); i++; continue; }
        var op = {"+": "+", "−": "-", "-": "-", "×": "*", "*": "*", "÷": "/", "/": "/", "^": "^",
                  "(": "(", ")": ")", "!": "!", "%": "%"}[c];
        if (!op) return null;
        tokens.push({kind: "op", value: op});
        i++;
    }
    return tokens;
}

// Results of trigonometry that are a rounding error away from a half or a
// whole (sin 30°, cos 90°) are that value.
function snap(x) {
    var half = Math.round(x * 2) / 2;
    return Math.abs(x - half) < 1e-12 ? half : x;
}

function factorial(n) {
    if (n < 0 || n !== Math.floor(n) || n > 170) return NaN;
    var r = 1;
    for (var i = 2; i <= n; i++) r *= i;
    return r;
}

function apply(name, x, degrees) {
    var angle = degrees ? x * Math.PI / 180 : x;
    switch (name) {
    case "sin": return snap(Math.sin(angle));
    case "cos": return snap(Math.cos(angle));
    case "tan":
        if (degrees && Math.abs((x % 180 + 180) % 180 - 90) < 1e-9) return NaN;
        return snap(Math.tan(angle));
    case "ln": return Math.log(x);
    case "log": return Math.log10(x);
    case "√": return Math.sqrt(x);
    }
    return NaN;
}

// The value of TEXT, or NaN when it is incomplete or not a number (a
// division by zero, a square root of a negative).
function evaluate(text, degrees) {
    var tokens = tokenize(text);
    if (!tokens || !tokens.length) return NaN;
    var pos = 0;
    function peek() { return tokens[pos]; }
    function isOp(value) { var t = tokens[pos]; return t && t.kind === "op" && t.value === value; }
    function startsPrimary() {
        var t = tokens[pos];
        return t && (t.kind === "number" || t.kind === "function" || (t.kind === "op" && t.value === "("));
    }
    function expression() {
        var value = term();
        while (isOp("+") || isOp("-")) {
            var op = tokens[pos++].value;
            var right = term();
            value = op === "+" ? value + right : value - right;
        }
        return value;
    }
    function term() {
        var value = signed();
        for (;;) {
            if (isOp("*") || isOp("/")) {
                var op = tokens[pos++].value;
                var right = signed();
                value = op === "*" ? value * right : value / right;
            } else if (startsPrimary()) {
                value *= signed();
            } else {
                return value;
            }
        }
    }
    function signed() {
        if (isOp("-")) { pos++; return -signed(); }
        if (isOp("+")) { pos++; return signed(); }
        return power();
    }
    function power() {
        var base = postfix();
        if (isOp("^")) { pos++; return Math.pow(base, signed()); }
        return base;
    }
    function postfix() {
        var value = primary();
        while (isOp("!") || isOp("%")) {
            value = tokens[pos++].value === "!" ? factorial(value) : value / 100;
        }
        return value;
    }
    function primary() {
        var t = peek();
        if (!t) throw "incomplete";
        if (t.kind === "number") { pos++; return t.value; }
        // A function takes its parenthesis (or the number right after it):
        // sin(30)^2 is (sin 30)².
        if (t.kind === "function") { pos++; return apply(t.value, primary(), degrees); }
        if (isOp("(")) {
            pos++;
            var value = expression();
            if (isOp(")")) pos++;
            else if (pos < tokens.length) throw "unbalanced";
            return value;
        }
        throw "unexpected";
    }
    try {
        var value = expression();
        if (pos !== tokens.length) return NaN;
        return value;
    } catch (e) {
        return NaN;
    }
}

// A number for the display and for typing on with: fifteen significant
// digits (binary rounding shows from the sixteenth: 0.1+0.2 is 0.3);
// scientific notation (1.5E20) when very large or small; − for minus; ""
// for no number.
function format(value) {
    if (typeof value !== "number" || !isFinite(value)) return "";
    if (value === 0) return "0";
    var size = Math.abs(value);
    var text;
    if (size >= 1e15 || size < 1e-6) {
        var parts = value.toExponential(9).split("e");
        text = parts[0].replace(/\.?0+$/, "") + "E" + parseInt(parts[1], 10);
    } else {
        text = String(parseFloat(value.toPrecision(15)));
    }
    return text.replace(/-/g, "−");
}

// Thousands separators in the whole part of every plain number in TEXT.
function grouped(text) {
    return text.replace(/\d+(\.\d*)?(E[+−-]?\d+)?/g, function (number) {
        if (number.indexOf("E") >= 0) return number;
        var dot = number.indexOf(".");
        var whole = dot < 0 ? number : number.slice(0, dot);
        var rest = dot < 0 ? "" : number.slice(dot);
        return whole.replace(/\B(?=(\d{3})+(?!\d))/g, ",") + rest;
    });
}
