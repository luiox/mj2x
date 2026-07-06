package ssa_cases;

public class SsaNarrowingCases {
  public byte b;
  public short s;
  public char c;

  public static int narrowMath(int x) {
    byte b = (byte)(x - 130);
    short s = (short)(x * 3);
    char c = (char)(x + 65);
    return b + s + c;
  }

  public static int narrowFields(SsaNarrowingCases box, int x) {
    box.b = (byte)(x - 130);
    box.s = (short)(x * 3);
    box.c = (char)(x + 65);
    return box.b + box.s + box.c;
  }
}
