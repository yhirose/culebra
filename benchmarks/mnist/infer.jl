#!/usr/bin/env julia
# Julia inference benchmark — same forward pass as infer_numpy.py.

using DelimitedFiles, LinearAlgebra, Statistics

const DIR = @__DIR__
const CYCLES = 3

sigmoid(z) = 1.0 ./ (1.0 .+ exp.(.-z))

function bench()
    t0 = time()
    W1 = readdlm(joinpath(DIR, "W1.csv"), ',', Float64)
    b1 = readdlm(joinpath(DIR, "b1.csv"), ',', Float64)
    W2 = readdlm(joinpath(DIR, "W2.csv"), ',', Float64)
    b2 = readdlm(joinpath(DIR, "b2.csv"), ',', Float64)
    X  = readdlm(joinpath(DIR, "test_images.csv"), ',', Float64)
    y  = vec(readdlm(joinpath(DIR, "test_labels.csv"), ',', Int64))
    t_load = time() - t0

    n = size(X, 1)
    println("[julia] loaded N=$n in $(round(t_load, digits=3))s")

    times = Float64[]
    local preds
    for _ in 1:CYCLES
        t0 = time()
        Xt = transpose(X)               # (784, N)
        z1 = W1 * Xt .+ b1              # (30, N)
        a1 = sigmoid(z1)
        z2 = W2 * a1 .+ b2              # (10, N)
        a2 = sigmoid(z2)
        preds = vec(getindex.(argmax(a2; dims=1), 1)) .- 1   # 1-indexed -> 0-indexed
        push!(times, time() - t0)
    end

    acc = mean(preds .== y)
    cold = times[1]
    warm = length(times) > 1 ? mean(times[2:end]) : NaN
    r(x) = round(x, digits=4)
    println("[julia] cold=$(r(cold))s warm=$(r(warm))s accuracy=$(r(acc))")
    println("BENCH label=julia_infer load=$(r(t_load)) cold=$(r(cold)) warm=$(r(warm)) accuracy=$(r(acc))")
end

bench()
