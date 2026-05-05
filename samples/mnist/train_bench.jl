#!/usr/bin/env julia
# Julia training benchmark — same hand-coded backprop as train_bench_numpy.py.

using DelimitedFiles, LinearAlgebra, Statistics

const DIR = @__DIR__
const BATCH = 10
const ETA = 3.0
const N_OUT = 10
const CYCLES = 2

sigmoid(z) = 1.0 ./ (1.0 .+ exp.(.-z))

function run_epoch!(W1, b1, W2, b2, X, Y, n)
    eta_over_batch = ETA / BATCH
    s = 1
    while s <= n
        e = min(s + BATCH - 1, n)
        x = transpose(X[s:e, :])         # (784, BATCH)
        t = transpose(Y[s:e, :])         # (10, BATCH)

        z1 = W1 * x .+ b1
        a1 = sigmoid(z1)
        z2 = W2 * a1 .+ b2
        a2 = sigmoid(z2)

        d2 = (a2 .- t) .* a2 .* (1.0 .- a2)
        d1 = (transpose(W2) * d2) .* a1 .* (1.0 .- a1)

        W2 .-= eta_over_batch .* (d2 * transpose(a1))
        b2 .-= ETA .* mean(d2; dims=2)
        W1 .-= eta_over_batch .* (d1 * transpose(x))
        b1 .-= ETA .* mean(d1; dims=2)

        s = e + 1
    end
end

function bench()
    t0 = time()
    init_W1 = readdlm(joinpath(DIR, "init_W1.csv"), ',', Float64)
    init_b1 = readdlm(joinpath(DIR, "init_b1.csv"), ',', Float64)
    init_W2 = readdlm(joinpath(DIR, "init_W2.csv"), ',', Float64)
    init_b2 = readdlm(joinpath(DIR, "init_b2.csv"), ',', Float64)
    X  = readdlm(joinpath(DIR, "train_images.csv"), ',', Float64)
    y  = vec(readdlm(joinpath(DIR, "train_labels.csv"), ',', Int64))
    Xt = readdlm(joinpath(DIR, "test_images.csv"), ',', Float64)
    yt = vec(readdlm(joinpath(DIR, "test_labels.csv"), ',', Int64))
    t_load = time() - t0

    n = size(X, 1)
    Y = zeros(Float64, n, N_OUT)
    for i in 1:n
        Y[i, y[i] + 1] = 1.0       # 0-indexed labels -> 1-indexed columns
    end

    println("[julia-train] loaded train=$n test=$(size(Xt, 1)) in $(round(t_load, digits=3))s")

    times = Float64[]
    local W1, b1, W2, b2
    for _ in 1:CYCLES
        W1 = copy(init_W1)
        b1 = copy(init_b1)
        W2 = copy(init_W2)
        b2 = copy(init_b2)
        t0 = time()
        run_epoch!(W1, b1, W2, b2, X, Y, n)
        push!(times, time() - t0)
    end

    z1 = W1 * transpose(Xt) .+ b1
    a1 = sigmoid(z1)
    z2 = W2 * a1 .+ b2
    a2 = sigmoid(z2)
    preds = vec(getindex.(argmax(a2; dims=1), 1)) .- 1
    acc = mean(preds .== yt)

    cold = times[1]
    warm = length(times) > 1 ? mean(times[2:end]) : NaN
    r(x) = round(x, digits=4)
    println("[julia-train] cold=$(r(cold))s warm=$(r(warm))s accuracy=$(r(acc))")
    println("BENCH label=julia_train load=$(r(t_load)) cold=$(r(cold)) warm=$(r(warm)) accuracy=$(r(acc))")
end

bench()
