VAR a, b, r;

PROCEDURE outer;
VAR c;

  PROCEDURE middle;
  CONST k = 10;
  VAR d;

    PROCEDURE inner;
    BEGIN
      a := a + k;
      d := d + 1;
      c := c + 2
    END;

  BEGIN
    d := 100;
    CALL inner;
    r := a + c + d
  END;

BEGIN
  c := 5;
  CALL middle
END;

PROCEDURE first;
BEGIN
  a := a * 2;
  CALL second
END;

PROCEDURE second;
BEGIN
  b := a + 1
END;

BEGIN
  a := 1;
  CALL outer;
  ! r;
  CALL first;
  ! a;
  ! b
END.
