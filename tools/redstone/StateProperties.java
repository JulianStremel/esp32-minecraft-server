// Runtime queries against the checksum-pinned official Java 1.16.5 server jar.
// Obfuscated identifiers are resolved using Mojang's 1.16.5 server mappings.
// No game server is started and no decompiled source is used in this helper.
import java.lang.reflect.Proxy;
import java.io.PrintWriter;
import java.io.InputStreamReader;
import java.util.*;
import java.util.jar.JarFile;
import com.google.gson.JsonParser;

class StateProperties {
    public static void main(String[] args) throws Exception {
        w.a(); vm.a(); // SharedConstants.getCurrentVersion; Bootstrap.bootStrap
        // Bootstrap registers blocks, but resource tags are normally loaded by the
        // server's resource manager. Load the jar's actual block tags (including
        // nested references) before state queries such as note instruments.
        Map<vk, buo> blocks = new HashMap<>();
        for (int i = 0; i < buo.m.a(); i++) {
            buo b = buo.a(i).b();
            String name = b.toString();
            blocks.put(new vk(name.substring(6, name.length() - 1)), b);
        }
        Map<vk, ael.a> builders = new HashMap<>();
        try (JarFile jar = new JarFile(System.getProperty("java.class.path"))) {
            var entries = jar.entries();
            String prefix = "data/minecraft/tags/blocks/";
            while (entries.hasMoreElements()) {
                var entry = entries.nextElement();
                String name = entry.getName();
                if (!name.startsWith(prefix) || !name.endsWith(".json")) continue;
                try (var reader = new InputStreamReader(jar.getInputStream(entry), java.nio.charset.StandardCharsets.UTF_8)) {
                    builders.put(new vk(name.substring(prefix.length(), name.length() - 5)),
                            ael.a.a().a(new JsonParser().parse(reader).getAsJsonObject(), name));
                }
            }
        }
        aem<buo> blockTags = new aeo<buo>(id -> Optional.ofNullable(blocks.get(id)), "tags/blocks", "blocks").a(builders);
        aen.a(blockTags, aem.c(), aem.c(), aem.c()).e();
        brc empty = (brc) Proxy.newProxyInstance(brc.class.getClassLoader(), new Class[]{brc.class}, (p, m, a) -> {
            if (m.getName().equals("d_")) return buo.a(0); // getBlockState: air
            if (m.getReturnType().isInstance(buo.a(0).m())) return buo.a(0).m(); // empty fluid
            if (m.getName().equals("c")) return null; // no block entity
            throw new UnsupportedOperationException(m.toString());
        });
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int i = 0; i < buo.m.a(); i++) {
                ceh s = buo.a(i); // Block.stateById
                int flags = s.f(); // light emission
                if (s.g(empty, fx.b)) flags |= 16; // isRedstoneConductor
                if (s.i()) flags |= 32; // isSignalSource
                for (gc d : gc.values()) if (s.d(empty, fx.b, d)) flags |= 1 << (6 + d.c()); // isFaceSturdy
                flags |= cfh.a(s).ordinal() << 12; // note instrument selected by this support
                int piston = s.k().ordinal(); // PushReaction
                if (s.b().q()) piston |= 8; // block entity: never movable in Java
                if (s.h(empty, fx.b) == -1.0f) piston |= 16; // unbreakable
                StringJoiner boxes = new StringJoiner(";");
                for (dci box : s.k(empty, fx.b).d()) {
                    StringJoiner coords = new StringJoiner(",");
                    for (double v : new double[]{box.a, box.b, box.c, box.d, box.e, box.f}) {
                        // Vanilla block collision shapes use a 1/32 grid. Fail
                        // rather than silently round if that ever stops being true.
                        int q = (int)Math.round(v * 32);
                        if (Math.abs(q - v * 32) > 1e-6 || q < -128 || q > 127)
                            throw new IllegalStateException("Non-grid collision shape: " + s);
                        coords.add(Integer.toString(q));
                    }
                    boxes.add(coords.toString());
                }
                out.println(i + "\t" + flags + "\t" + s + "\t" + piston + "\t" + boxes);
            }
        }
    }
}
