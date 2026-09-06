function run(n) {
  let s = "";
  let i = 0;
  while (i < n) {
    s = s + "x";
    i = i + 1;
  }
  return s.length;
}
console.log(run(40000));
