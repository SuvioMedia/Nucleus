package dev.nucleusframework.desktop.application.internal.analyzer

import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ClassForNameDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ClassReferenceCollector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.JarResourceDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.KotlinSerializableDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.MethodHandleDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.NativeMethodDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.OrphanProjectClassDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ProjectClassFact
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ProxyDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ReflectionApiDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ResourceAccessDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ResourceBundleDetector
import dev.nucleusframework.desktop.application.internal.analyzer.detectors.ServiceLoaderDetector
import java.io.File
import java.util.jar.JarFile

/**
 * Main entry point: scans one or more JARs / class directories and produces an [AnalysisResult].
 *
 * When [detectOrphanProjectClasses] or [reflectionForProjectClasses] is enabled, a single
 * classpath walk also collects type-use references and project class header facts, then
 * runs [OrphanProjectClassDetector] — no second JAR pass (#441).
 */
internal object BytecodeAnalyzer {
    /**
     * Analyzes a single JAR file.
     */
    fun analyzeJar(
        jarPath: File,
        excludedTypePrefixes: Set<String> = emptySet(),
        excludedResourceGlobs: Set<String> = emptySet(),
    ): AnalysisResult {
        val exclusions = StaticMetadataExclusions(excludedTypePrefixes, excludedResourceGlobs)
        return analyzeJarInternal(jarPath, collectReferences = false, exclusions.typeMatcher)
            .toResult()
            .excluding(exclusions)
    }

    /**
     * Analyzes a directory of compiled .class files (e.g. build/classes/kotlin/jvm/main).
     */
    fun analyzeClassDir(
        dir: File,
        excludedTypePrefixes: Set<String> = emptySet(),
    ): AnalysisResult {
        val exclusions = StaticMetadataExclusions(excludedTypePrefixes, emptySet())
        return analyzeClassDirInternal(
            dir = dir,
            collectReferences = false,
            collectProjectFacts = false,
            excludedTypes = exclusions.typeMatcher,
        ).toResult().excluding(exclusions)
    }

    /**
     * Analyzes a classpath that may contain both JARs and class directories.
     *
     * @param projectClassDirs the app's own class output dirs (must not include dependency JARs).
     *   Used only when orphan / all-project registration is enabled.
     * @param detectOrphanProjectClasses register public no-arg `<init>` for project classes that
     *   no bytecode references (default path for Room `_Impl` and friends).
     * @param reflectionForProjectClasses sledgehammer: register every project class with a
     *   public no-arg ctor. When true, takes precedence over the orphan rule (same registrations
     *   are a superset).
     */
    fun analyzeClasspath(
        files: Iterable<File>,
        projectClassDirs: Collection<File> = emptyList(),
        detectOrphanProjectClasses: Boolean = false,
        reflectionForProjectClasses: Boolean = false,
        excludedTypePrefixes: Set<String> = emptySet(),
        excludedResourceGlobs: Set<String> = emptySet(),
    ): AnalysisResult {
        val exclusions = StaticMetadataExclusions(excludedTypePrefixes, excludedResourceGlobs)
        val needProjectFacts = detectOrphanProjectClasses || reflectionForProjectClasses
        val needReferences = detectOrphanProjectClasses // all-project path does not need refs
        if (!needProjectFacts) {
            return analyzeClasspathLegacy(files, exclusions)
        }

        val projectDirSet =
            projectClassDirs
                .filter { it.isDirectory && it.exists() }
                .map { it.canonicalFile }
                .toSet()

        val allReferenced = mutableSetOf<String>()
        val appReferenced = mutableSetOf<String>()
        val projectFacts = mutableListOf<ProjectClassFact>()
        val scannedProjectDirs = mutableSetOf<File>()
        var merged = AnalysisResult()

        for (file in files) {
            when {
                file.isDirectory && file.exists() -> {
                    val canonical = file.canonicalFile
                    val isProject =
                        projectDirSet.any { projectDir ->
                            canonical == projectDir || canonical.toPath().startsWith(projectDir.toPath())
                        }
                    val partial =
                        analyzeClassDirInternal(
                            dir = file,
                            collectReferences = needReferences,
                            collectProjectFacts = isProject,
                            excludedTypes = exclusions.typeMatcher,
                        )
                    merged += partial.toResult()
                    if (needReferences) {
                        allReferenced += partial.referencedTypes
                        if (isProject) {
                            appReferenced += partial.referencedTypes
                        }
                    }
                    if (isProject) {
                        projectFacts += partial.projectFacts
                        scannedProjectDirs += canonical
                    }
                }
                file.isFile && file.name.endsWith(".jar") && file.exists() -> {
                    val partial =
                        analyzeJarInternal(
                            jarPath = file,
                            collectReferences = needReferences,
                            excludedTypes = exclusions.typeMatcher,
                        )
                    merged += partial.toResult()
                    if (needReferences) {
                        allReferenced += partial.referencedTypes
                    }
                }
            }
        }

        // Project dirs not already present on the runtime classpath (unusual, but cheap)
        for (projectDir in projectDirSet) {
            if (projectDir in scannedProjectDirs) continue
            val partial =
                analyzeClassDirInternal(
                    dir = projectDir,
                    collectReferences = true,
                    collectProjectFacts = true,
                    excludedTypes = exclusions.typeMatcher,
                )
            merged += partial.toResult()
            allReferenced += partial.referencedTypes
            appReferenced += partial.referencedTypes
            projectFacts += partial.projectFacts
        }

        val projectEntries =
            when {
                reflectionForProjectClasses -> OrphanProjectClassDetector.detectAll(projectFacts)
                detectOrphanProjectClasses ->
                    OrphanProjectClassDetector.detect(
                        projectFacts = projectFacts,
                        classpathReferencedTypes = allReferenced,
                        appReferencedTypes = appReferenced,
                    )
                else -> emptySet()
            }

        return merged.copy(projectClassEntries = projectEntries).excluding(exclusions)
    }

    /**
     * Analyzes multiple JARs and merges results.
     */
    fun analyzeJars(jarPaths: List<File>): AnalysisResult {
        var merged = AnalysisResult()
        for (jar in jarPaths) {
            merged = merged + analyzeJar(jar)
        }
        return merged
    }

    private fun analyzeClasspathLegacy(
        files: Iterable<File>,
        exclusions: StaticMetadataExclusions,
    ): AnalysisResult {
        var merged = AnalysisResult()
        for (file in files) {
            merged = merged +
                when {
                    file.isDirectory ->
                        analyzeClassDirInternal(
                            dir = file,
                            collectReferences = false,
                            collectProjectFacts = false,
                            excludedTypes = exclusions.typeMatcher,
                        ).toResult()
                    file.isFile && file.name.endsWith(".jar") ->
                        analyzeJarInternal(
                            jarPath = file,
                            collectReferences = false,
                            excludedTypes = exclusions.typeMatcher,
                        ).toResult()
                    else -> continue
                }
        }
        return merged.excluding(exclusions)
    }

    private data class PartialScan(
        val reflectionEntries: Set<ReflectionEntry> = emptySet(),
        val jniEntries: Set<JniEntry> = emptySet(),
        val resourcePatterns: Set<ResourcePattern> = emptySet(),
        val serviceLoaderEntries: Set<ReflectionEntry> = emptySet(),
        val referencedTypes: Set<String> = emptySet(),
        val projectFacts: List<ProjectClassFact> = emptyList(),
    ) {
        fun toResult(): AnalysisResult =
            AnalysisResult(
                reflectionEntries = reflectionEntries,
                jniEntries = jniEntries,
                resourcePatterns = resourcePatterns,
                serviceLoaderEntries = serviceLoaderEntries,
            )
    }

    private fun analyzeJarInternal(
        jarPath: File,
        collectReferences: Boolean,
        excludedTypes: TypePrefixMatcher = TypePrefixMatcher(emptySet()),
    ): PartialScan {
        if (!jarPath.exists() || !jarPath.name.endsWith(".jar")) {
            return PartialScan()
        }

        val jniEntries = mutableSetOf<JniEntry>()
        val reflectionEntries = mutableSetOf<ReflectionEntry>()
        val resourcePatterns = mutableSetOf<ResourcePattern>()
        val serviceLoaderEntries = mutableSetOf<ReflectionEntry>()
        val jniReferencedTypes = mutableSetOf<String>()
        val jniFieldTypes = mutableSetOf<String>()
        val jniSuperclassTypes = mutableSetOf<String>()
        val classBytesIndex = mutableMapOf<String, ByteArray>()
        val referencedTypes = mutableSetOf<String>()

        try {
            JarFile(jarPath).use { jar ->
                val serviceResult = ServiceLoaderDetector.detect(jar)
                serviceLoaderEntries.addAll(serviceResult.reflectionEntries)
                resourcePatterns.addAll(serviceResult.resourcePatterns)
                resourcePatterns.addAll(JarResourceDetector.detect(jar))

                for (entry in jar.entries()) {
                    if (!entry.name.endsWith(".class") || entry.name.startsWith("META-INF/")) continue

                    val internalName = entry.name.removeSuffix(".class")
                    if (excludedTypes.matches(internalName.replace('/', '.'))) continue

                    val classBytes =
                        try {
                            jar.getInputStream(entry).use { it.readBytes() }
                        } catch (_: Exception) {
                            continue
                        }
                    classBytesIndex[internalName] = classBytes

                    try {
                        val nativeResult = NativeMethodDetector.detectWithReferences(classBytes)
                        jniEntries.addAll(nativeResult.jniEntries)
                        jniReferencedTypes.addAll(nativeResult.referencedTypes)
                        jniFieldTypes.addAll(nativeResult.jniClassFieldTypes)
                        nativeResult.superclassType?.let { jniSuperclassTypes.add(it) }

                        analyzeClassBytes(classBytes, reflectionEntries, resourcePatterns)
                        if (collectReferences) {
                            // Nest-internal edges (Outer$Nested → Outer) must not hide Room
                            // orphans; see OrphanProjectClassDetector.addExternalReferences.
                            OrphanProjectClassDetector.addExternalReferences(
                                sourceFqcn = internalName.replace('/', '.'),
                                refs = ClassReferenceCollector.collect(classBytes),
                                into = referencedTypes,
                            )
                        }
                    } catch (_: IllegalArgumentException) {
                        // ASM does not support this class file version (e.g. JDK 25+) — skip
                    }
                }

                val allJniCallbackCandidates = jniFieldTypes + jniReferencedTypes + jniSuperclassTypes
                resolveJniCallbackTypes(allJniCallbackCandidates, classBytesIndex, jniEntries)
                enrichJniClassEntries(classBytesIndex, jniEntries)
            }
        } catch (_: Exception) {
            // Corrupt JAR — return whatever was collected
        }

        for (refType in jniReferencedTypes) {
            if (jniEntries.none { it.type == refType }) {
                jniEntries.add(JniEntry(type = refType))
            }
        }

        return PartialScan(
            reflectionEntries = reflectionEntries,
            jniEntries = jniEntries,
            resourcePatterns = resourcePatterns,
            serviceLoaderEntries = serviceLoaderEntries,
            referencedTypes = referencedTypes,
        )
    }

    private fun analyzeClassDirInternal(
        dir: File,
        collectReferences: Boolean,
        collectProjectFacts: Boolean,
        excludedTypes: TypePrefixMatcher = TypePrefixMatcher(emptySet()),
    ): PartialScan {
        if (!dir.exists() || !dir.isDirectory) return PartialScan()

        val jniEntries = mutableSetOf<JniEntry>()
        val reflectionEntries = mutableSetOf<ReflectionEntry>()
        val resourcePatterns = mutableSetOf<ResourcePattern>()
        val serviceLoaderEntries = mutableSetOf<ReflectionEntry>()
        val jniReferencedTypes = mutableSetOf<String>()
        val jniFieldTypes = mutableSetOf<String>()
        val jniSuperclassTypes = mutableSetOf<String>()
        val classBytesIndex = mutableMapOf<String, ByteArray>()
        val referencedTypes = mutableSetOf<String>()
        val projectFacts = mutableListOf<ProjectClassFact>()

        val servicesDir = File(dir, "META-INF/services")
        if (servicesDir.isDirectory) {
            servicesDir.listFiles()?.filter { it.isFile }?.forEach { serviceFile ->
                val serviceName = serviceFile.name
                resourcePatterns.add(ResourcePattern(glob = "META-INF/services/$serviceName"))
                val implementations =
                    serviceFile
                        .readLines()
                        .map { it.substringBefore('#').trim() }
                        .filter { it.isNotEmpty() }
                for (impl in implementations) {
                    serviceLoaderEntries.add(
                        ReflectionEntry(
                            type = impl,
                            methods = setOf(MethodSignature("<init>", emptyList())),
                        ),
                    )
                }
            }
        }

        dir
            .walkTopDown()
            .filter { it.isFile && it.extension == "class" }
            .forEach { classFile ->
                val relativePath =
                    classFile
                        .relativeTo(dir)
                        .path
                        .removeSuffix(".class")
                        .replace('\\', '/')
                if (excludedTypes.matches(relativePath.replace('/', '.'))) return@forEach

                val classBytes =
                    try {
                        classFile.readBytes()
                    } catch (_: Exception) {
                        return@forEach
                    }
                classBytesIndex[relativePath] = classBytes

                try {
                    val nativeResult = NativeMethodDetector.detectWithReferences(classBytes)
                    jniEntries.addAll(nativeResult.jniEntries)
                    jniReferencedTypes.addAll(nativeResult.referencedTypes)
                    jniFieldTypes.addAll(nativeResult.jniClassFieldTypes)
                    nativeResult.superclassType?.let { jniSuperclassTypes.add(it) }

                    analyzeClassBytes(classBytes, reflectionEntries, resourcePatterns)
                    if (collectReferences) {
                        OrphanProjectClassDetector.addExternalReferences(
                            sourceFqcn = relativePath.replace('/', '.'),
                            refs = ClassReferenceCollector.collect(classBytes),
                            into = referencedTypes,
                        )
                    }
                    if (collectProjectFacts) {
                        OrphanProjectClassDetector.inspect(classBytes)?.let { projectFacts.add(it) }
                    }
                } catch (_: IllegalArgumentException) {
                    // ASM does not support this class file version (e.g. JDK 25+) — skip
                }
            }

        val allJniCallbackCandidates = jniFieldTypes + jniReferencedTypes + jniSuperclassTypes
        resolveJniCallbackTypes(allJniCallbackCandidates, classBytesIndex, jniEntries)
        enrichJniClassEntries(classBytesIndex, jniEntries)

        for (refType in jniReferencedTypes) {
            if (jniEntries.none { it.type == refType }) {
                jniEntries.add(JniEntry(type = refType))
            }
        }

        return PartialScan(
            reflectionEntries = reflectionEntries,
            jniEntries = jniEntries,
            resourcePatterns = resourcePatterns,
            serviceLoaderEntries = serviceLoaderEntries,
            referencedTypes = referencedTypes,
            projectFacts = projectFacts,
        )
    }

    private fun resolveJniCallbackTypes(
        callbackCandidates: Set<String>,
        classBytesIndex: Map<String, ByteArray>,
        jniEntries: MutableSet<JniEntry>,
    ) {
        val expandedCandidates = mutableSetOf<String>()
        expandedCandidates.addAll(callbackCandidates)

        for (typeName in callbackCandidates) {
            val internalPrefix = typeName.replace('.', '/') + "$"
            for (key in classBytesIndex.keys) {
                if (key.startsWith(internalPrefix)) {
                    expandedCandidates.add(key.replace('/', '.'))
                }
            }
        }

        for (typeName in expandedCandidates) {
            if (typeName.startsWith("java.") || typeName.startsWith("javax.")) continue
            if (jniEntries.any { it.type == typeName && it.methods.isNotEmpty() }) continue

            val internalName = typeName.replace('.', '/')
            val classBytes = classBytesIndex[internalName] ?: continue

            val callbackEntry =
                try {
                    NativeMethodDetector.extractJniCallbackEntry(classBytes)
                } catch (_: IllegalArgumentException) {
                    continue
                }
            if (callbackEntry != null && (callbackEntry.methods.isNotEmpty() || callbackEntry.fields.isNotEmpty())) {
                jniEntries.removeAll { it.type == typeName }
                jniEntries.add(callbackEntry)
            }
        }
    }

    private fun enrichJniClassEntries(
        classBytesIndex: Map<String, ByteArray>,
        jniEntries: MutableSet<JniEntry>,
    ) {
        val nativeClassTypes =
            jniEntries
                .filter { it.methods.isNotEmpty() && !it.jniAccessible }
                .map { it.type }
                .toList()

        for (typeName in nativeClassTypes) {
            val internalName = typeName.replace('.', '/')
            val classBytes = classBytesIndex[internalName] ?: continue

            val fullEntry =
                try {
                    NativeMethodDetector.extractJniCallbackEntry(classBytes) ?: continue
                } catch (_: IllegalArgumentException) {
                    continue
                }

            val existingEntry = jniEntries.first { it.type == typeName }
            val mergedMethods = existingEntry.methods + fullEntry.methods
            val mergedFields = fullEntry.fields

            jniEntries.remove(existingEntry)
            jniEntries.add(
                JniEntry(
                    type = typeName,
                    methods = mergedMethods,
                    fields = mergedFields,
                    jniAccessible = true,
                ),
            )
        }
    }

    private fun analyzeClassBytes(
        classBytes: ByteArray,
        reflectionEntries: MutableSet<ReflectionEntry>,
        resourcePatterns: MutableSet<ResourcePattern>,
    ) {
        reflectionEntries.addAll(ClassForNameDetector.detect(classBytes))
        reflectionEntries.addAll(ReflectionApiDetector.detect(classBytes))
        resourcePatterns.addAll(ResourceBundleDetector.detect(classBytes))
        resourcePatterns.addAll(ResourceAccessDetector.detect(classBytes))
        reflectionEntries.addAll(MethodHandleDetector.detect(classBytes))
        reflectionEntries.addAll(ProxyDetector.detect(classBytes))
        reflectionEntries.addAll(KotlinSerializableDetector.detect(classBytes))
    }
}
