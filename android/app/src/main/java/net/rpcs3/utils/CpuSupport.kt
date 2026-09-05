package net.rpcs3.utils

import java.io.File

object CpuSupport {
    private val required = listOf(
        "atomics" to "LSE atomics",
        "asimdrdm" to "RDMA",
        "fphp" to "FP16",
        "asimdhp" to "FP16 SIMD",
        "asimddp" to "dot product",
    )

    val missingFeatures: String? by lazy { detect() }

    private fun detect(): String? {
        val perCore = runCatching {
            File("/proc/cpuinfo").readLines()
                .filter { it.trimStart().startsWith("Features") }
                .map { line -> line.substringAfter(':').trim().split(' ').filter { it.isNotEmpty() }.toSet() }
        }.getOrNull().orEmpty()

        if (perCore.isEmpty()) return null

        val missing = required.filter { (flag, _) -> perCore.any { flag !in it } }.map { it.second }
        if (missing.isEmpty()) return null

        return missing.joinToString(", ")
    }
}
