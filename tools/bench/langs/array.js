function run(n) {
  let xs = [];
  let i = 1;
  while (i <= n) {
    xs.push(i * i);
    i = i + 1;
  }
  let total = 0;
  let j = 0;
  while (j < xs.length) {
    total = (total + xs[j]) % 1000000007;
    j = j + 1;
  }
  return total;
}
console.log(run(500000));
