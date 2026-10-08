import QtQuick
import QtTest
import "calc.js" as Calc

// The calculator's arithmetic (overlay/mobile/apps/calculator/calc.js).
TestCase {
    name: "Calc"

    function near(text, want, degrees) {
        const got = Calc.evaluate(text, degrees === true);
        verify(Math.abs(got - want) < 1e-9, text + " gave " + got + ", not " + want);
    }

    function test_precedence_and_signs() {
        near("1+2×3", 7);
        near("(1+2)×3", 9);
        near("2^3^2", 512);
        near("−2^2", -4);
        near("2^−1", 0.5);
        near("5−−3", 8);
        near("10÷4", 2.5);
    }
    function test_juxtaposition_and_open_parentheses() {
        near("2π", 2 * Math.PI);
        near("3(4)", 12);
        near("2(3)(4)", 24);
        near("2×(3+4", 14);
    }
    function test_postfix_and_functions() {
        near("50%", 0.5);
        near("5!", 120);
        near("sin(30)", 0.5, true);
        near("cos(90)", 0, true);
        near("sin(30)^2", 0.25, true);
        near("√(16)", 4);
        near("ln(e)", 1);
        near("log(1000)", 3);
        near("1.5E20×2", 3e20);
        near("1E−3", 0.001);
    }
    function test_no_number() {
        verify(isNaN(Calc.evaluate("3+", false)));
        verify(isNaN(Calc.evaluate("", false)));
        verify(isNaN(Calc.evaluate("tan(90)", true)));
        verify(isNaN(Calc.evaluate("2)", false)));
        verify(!isFinite(Calc.evaluate("1÷0", false)));
        verify(isNaN(Calc.evaluate("alert(1)", false)));
    }
    function test_format() {
        compare(Calc.format(0.1 + 0.2), "0.3");
        compare(Calc.format(1 / 3), "0.333333333333333");
        compare(Calc.format(123456789012345), "123456789012345");
        compare(Calc.format(3e20), "3E20");
        compare(Calc.format(-2.5e-8), "−2.5E−8");
        compare(Calc.format(Infinity), "");
        // A formatted result types on as a number.
        compare(Calc.evaluate(Calc.format(-2.5e-8) + "×2", false), -5e-8);
    }
    function test_grouping() {
        compare(Calc.grouped("1234567.891+12×1000"), "1,234,567.891+12×1,000");
        compare(Calc.grouped("1.5E20"), "1.5E20");
    }
}
