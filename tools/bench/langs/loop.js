function run(n) {
  let total = 0;
  let i = 1;
  while (i <= n) {
    total = (total + i * i) % 1000000007;
    i = i + 1;
  }
  return total;
}
console.log(run(Number(process.argv[2])));
