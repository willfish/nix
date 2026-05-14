# Common Docker Development Issues & Fixes

## "Database is not ready" errors

**Cause**: Web process starts before Postgres is accepting connections.

**Fix**: Use a proper entrypoint script + healthcheck, or `depends_on` with `condition: service_healthy`.

## Permission Denied on Files

**Cause**: Container runs as `root`, files created inside container are owned by root on the host (or vice versa).

**Fixes**:
- Match UID/GID in Dockerfile (`useradd -u $(id -u) ...`)
- Use `:cached` or `:delegated` on macOS
- Run commands as the host user

## Slow File Changes / Hot Reload Not Working

**Cause**: Bind mount performance issues (especially macOS).

**Fixes**:
- Use `docker-sync` or Mutagen
- Exclude `node_modules`, `tmp`, `log`, `.git` from volume where possible
- Use `spring` or `listen` gem carefully

## "Gem not found" after bundle install

**Cause**: `Gemfile.lock` was generated with a different Ruby/Bundler version, or gems were installed outside the container.

**Fix**: Always run `bundle install` inside the container after changing Gemfile.

## Out of Disk Space

**Cause**: Docker volumes, build cache, and old images accumulate.

**Fix**:
```bash
docker system prune -a --volumes
```

## "Address already in use" for port 3000

**Cause**: Another process (or previous failed container) is still using the port.

**Fix**:
```bash
docker compose down
# or
lsof -i :3000
```

## Debugging Tips

- `docker compose exec web bash`
- `docker compose logs -f web`
- `docker compose run --rm web bundle exec rails console`

These commands are invaluable when things go wrong inside the container.
