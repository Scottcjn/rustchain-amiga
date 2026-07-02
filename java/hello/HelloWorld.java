/*
 * HelloWorld.java - acceptance test for mjvm on m68k AmigaOS.
 * RustChain Amiga Edition, Phase 3 java/ workstream.
 *
 * Compile on the host with:  javac --release 8 HelloWorld.java
 * Run on the Amiga with:     mjvm HelloWorld.class
 *
 * Written inside the mjvm subset on purpose:
 *  - no string concatenation (that would need StringBuilder objects)
 *  - int math only (no long/float/double)
 *  - recursion, loops, iinc, and an int[] array to prove real bytecode
 *    execution, not canned output.
 */
public class HelloWorld {

    /* recursive: exercises invokestatic, if_icmpge, iadd/isub, ireturn */
    static int fib(int n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }

    /* loops + int[]: exercises newarray, iastore, iaload, iinc,
     * arraylength, if_icmplt/goto */
    static int sumOfSquares(int count) {
        int[] a = new int[count];
        for (int i = 0; i < a.length; i++) {
            a[i] = i * i;
        }
        int s = 0;
        for (int i = 0; i < a.length; i++) {
            s += a[i];
        }
        return s;
    }

    /* switch: exercises tableswitch/lookupswitch */
    static int pick(int n) {
        switch (n) {
            case 1:  return 10;
            case 2:  return 20;
            case 7:  return 70;
            default: return -1;
        }
    }

    public static void main(String[] args) {
        System.out.println("RUSTCHAIN-JAVA-MARKER: mjvm running Java bytecode on AmigaOS");
        System.out.print("fib(20)=");
        System.out.println(fib(20));          // expect 6765
        System.out.print("sumOfSquares(10)=");
        System.out.println(sumOfSquares(10)); // expect 285
        System.out.print("pick(7)=");
        System.out.println(pick(7));          // expect 70
        System.out.print("pick(3)=");
        System.out.println(pick(3));          // expect -1
        System.out.println("JAVA-TEST-COMPLETE");
    }
}
