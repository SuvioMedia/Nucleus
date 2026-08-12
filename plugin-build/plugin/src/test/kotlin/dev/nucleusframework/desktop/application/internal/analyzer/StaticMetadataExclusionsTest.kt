package dev.nucleusframework.desktop.application.internal.analyzer

import dev.nucleusframework.desktop.application.internal.filterMetadataEntries
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class StaticMetadataExclusionsTest {
    @Test
    fun `type prefixes match packages classes nested classes and arrays at boundaries`() {
        val matcher = TypePrefixMatcher(setOf("com.example.windows", "com.example.LinuxBridge"))

        assertTrue(matcher.matches("com.example.windows.Player"))
        assertTrue(matcher.matches("com.example.LinuxBridge"))
        assertTrue(matcher.matches("com.example.LinuxBridge\$Callback[]"))
        assertFalse(matcher.matches("com.example.windowsill.Player"))
        assertFalse(matcher.matches("com.example.LinuxBridgeFactory"))
    }

    @Test
    fun `filter removes excluded owners signatures and externally packaged resources`() {
        val result =
            AnalysisResult(
                reflectionEntries =
                    setOf(
                        ReflectionEntry(type = "com.example.windows.Player", allDeclaredMethods = true),
                        ReflectionEntry(
                            type = "com.example.Factory",
                            methods =
                                setOf(
                                    MethodSignature("windows", listOf("com.example.windows.Player")),
                                    MethodSignature("macos", listOf("java.lang.String")),
                                ),
                        ),
                    ),
                jniEntries =
                    setOf(
                        JniEntry(type = "com.example.LinuxBridge", jniAccessible = true),
                        JniEntry(
                            type = "com.example.SharedBridge",
                            methods =
                                setOf(
                                    MethodSignature("linux", listOf("com.example.LinuxBridge\$Callback[]")),
                                    MethodSignature("macos"),
                                ),
                        ),
                    ),
                resourcePatterns =
                    setOf(
                        ResourcePattern(glob = "libskiko-macos-arm64.dylib"),
                        ResourcePattern(glob = "nucleus/native/darwin-aarch64/libnucleus_tao.dylib"),
                    ),
            )

        val filtered =
            result.excluding(
                StaticMetadataExclusions(
                    typePrefixes = setOf("com.example.windows", "com.example.LinuxBridge"),
                    resourceGlobs = setOf("libskiko-macos-arm64.dylib"),
                ),
            )

        assertEquals(setOf("com.example.Factory"), filtered.reflectionEntries.mapTo(mutableSetOf()) { it.type })
        assertEquals(setOf("macos"), filtered.reflectionEntries.single().methods.mapTo(mutableSetOf()) { it.name })
        assertEquals(setOf("com.example.SharedBridge"), filtered.jniEntries.mapTo(mutableSetOf()) { it.type })
        assertEquals(setOf("macos"), filtered.jniEntries.single().methods.mapTo(mutableSetOf()) { it.name })
        assertEquals(
            setOf("nucleus/native/darwin-aarch64/libnucleus_tao.dylib"),
            filtered.resourcePatterns.mapNotNullTo(mutableSetOf()) { it.glob },
        )
    }

    @Test
    fun `library metadata filter removes excluded owners and parameter signatures`() {
        val entries =
            listOf(
                mapOf("type" to "com.example.windows.NativeBridge", "allDeclaredMethods" to true),
                mapOf(
                    "type" to "com.example.SharedBridge",
                    "methods" to
                        listOf(
                            mapOf("name" to "windows", "parameterTypes" to listOf("com.example.windows.Callback")),
                            mapOf("name" to "macos"),
                        ),
                ),
                mapOf("proxy" to listOf("com.example.Proxy")),
            )

        val filtered = filterMetadataEntries(entries, TypePrefixMatcher(setOf("com.example.windows")))

        assertEquals(2, filtered.size)
        @Suppress("UNCHECKED_CAST")
        val shared = filtered.first() as Map<String, Any?>
        @Suppress("UNCHECKED_CAST")
        val methods = shared["methods"] as List<Map<String, Any?>>
        assertEquals(listOf("macos"), methods.map { it["name"] })
        assertTrue(filtered.last() is Map<*, *>)
    }
}
