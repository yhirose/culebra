VAR i, c;
BEGIN
  i := 0;
  c := 0;
  WHILE i < 10 DO
  BEGIN
    IF ODD i THEN c := c + i;
    i := i + 1
  END;
  ! c;
  IF ODD 2 THEN ! 111;
  IF ODD 3 THEN ! 222
END.
