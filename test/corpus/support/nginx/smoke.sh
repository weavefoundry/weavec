#!/bin/sh
# The corpus gate's test of nginx (test/corpus/manifest.json, config nginx; nginx's own test
# suite is a separate repository): objs/nginx serves a directory on a free local port with two
# worker processes; static files of several sizes (a small one, one of 1 MiB, one past the
# output buffers), a missing file, a directory listing, a byte range and a HEAD request are
# fetched with curl and compared, and the server is stopped with `nginx -s quit`. Any
# difference, a request that fails, or a worker that dies (an error log line "exited on
# signal") fails the test.
set -u
root=$PWD/gate-nginx
rm -rf "$root"
mkdir -p "$root/conf" "$root/logs" "$root/html/dir" "$root/temp"
port=$((40000 + $$ % 2000))
cat > "$root/conf/nginx.conf" <<CONF
worker_processes 2;
error_log logs/error.log info;
pid logs/nginx.pid;
events { worker_connections 64; }
http {
  access_log logs/access.log;
  client_body_temp_path temp/body;
  proxy_temp_path temp/proxy;
  fastcgi_temp_path temp/fastcgi;
  uwsgi_temp_path temp/uwsgi;
  scgi_temp_path temp/scgi;
  server {
    listen 127.0.0.1:$port;
    root html;
    location /dir/ { autoindex on; }
  }
}
CONF
printf 'hello, nginx\n' > "$root/html/index.html"
head -c 1048576 /dev/urandom > "$root/html/big.bin"
head -c 300000 /dev/urandom > "$root/html/dir/mid.bin"
objs/nginx -p "$root" -c conf/nginx.conf || { cat "$root/logs/error.log"; exit 1; }
trap 'objs/nginx -p "$root" -c conf/nginx.conf -s quit 2>/dev/null' EXIT
status=0
fetch() { curl -sS --max-time 10 "$@"; }
sleep 1
url=http://127.0.0.1:$port
[ "$(fetch "$url/")" = "hello, nginx" ] || { echo "index differs"; status=1; }
fetch -o "$root/got.bin" "$url/big.bin" && cmp "$root/html/big.bin" "$root/got.bin" || { echo "big.bin differs"; status=1; }
fetch -o "$root/got-mid.bin" "$url/dir/mid.bin" && cmp "$root/html/dir/mid.bin" "$root/got-mid.bin" || { echo "mid.bin differs"; status=1; }
[ "$(fetch -o /dev/null -w '%{http_code}' "$url/missing")" = 404 ] || { echo "no 404"; status=1; }
fetch "$url/dir/" | grep -q 'mid.bin' || { echo "no listing"; status=1; }
[ "$(fetch -r 100-199 "$url/big.bin" | wc -c | tr -d ' ')" = 100 ] || { echo "range differs"; status=1; }
fetch -I "$url/big.bin" | grep -qi 'content-length: 1048576' || { echo "HEAD differs"; status=1; }
for i in 1 2 3 4 5 6 7 8; do fetch -o /dev/null "$url/big.bin" || status=1; done
objs/nginx -p "$root" -c conf/nginx.conf -s quit
trap - EXIT
sleep 1
if grep -E 'exited on signal|weavec:' "$root/logs/error.log"; then status=1; fi
exit $status
