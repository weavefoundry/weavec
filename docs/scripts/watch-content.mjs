// Regenerate imported Markdown when its maintained source changes in development.
import { execFile } from 'node:child_process';
import { readdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export function watchContent() {
  return {
    name: 'weavec-content-watch',
    hooks: {
      'astro:server:setup': ({ server, logger }) => {
        const docs = fileURLToPath(new URL('../', import.meta.url));
        const directories = ['pages', 'rfcs', 'examples', 'data'].map((name) =>
          path.join(docs, name),
        );
        const files = readdirSync(docs)
          .filter((name) => name.endsWith('.md'))
          .map((name) => path.join(docs, name));
        files.push(path.resolve(docs, '../CMakeLists.txt'), path.resolve(docs, '../CHANGELOG.md'));
        server.watcher.add([...directories, ...files]);
        let timer;
        let running = false;
        let pending = false;
        function prepare() {
          if (running) {
            pending = true;
            return;
          }
          running = true;
          execFile(
            process.execPath,
            [path.join(docs, 'scripts/prepare-content.mjs')],
            { cwd: docs },
            (error) => {
              running = false;
              if (error) logger.error(error.message);
              else server.ws.send({ type: 'full-reload' });
              if (pending) {
                pending = false;
                prepare();
              }
            },
          );
        }
        const change = (_event, file) => {
          if (
            !files.includes(file) &&
            !directories.some((directory) => file.startsWith(directory + path.sep))
          )
            return;
          clearTimeout(timer);
          timer = setTimeout(prepare, 150);
        };
        server.watcher.on('all', change);
        server.httpServer?.once('close', () => {
          clearTimeout(timer);
          server.watcher.off('all', change);
        });
      },
    },
  };
}
