---
name: docker
description: >
  Best practices for Docker-based development environments for Ruby on Rails + Sequel + Postgres + Redis + Sidekiq applications.
  Covers docker-compose setup, performance, common pitfalls, and developer experience improvements.
  Use when setting up or improving local development with Docker.
---

# Docker Development Environment Best Practices

Docker has become the standard way to run consistent development environments, especially when your stack includes Postgres, Redis, and background workers.

## Recommended docker-compose.yml Structure

```yaml
version: "3.8"

services:
  db:
    image: postgres:16
    volumes:
      - postgres_data:/var/lib/postgresql/data
    environment:
      POSTGRES_USER: app
      POSTGRES_PASSWORD: password
      POSTGRES_DB: myapp_development

  redis:
    image: redis:7-alpine
    volumes:
      - redis_data:/data

  web:
    build: .
    command: bin/rails server -b 0.0.0.0
    volumes:
      - .:/app
    ports:
      - "3000:3000"
    depends_on:
      - db
      - redis
    environment:
      DATABASE_URL: postgres://app:password@db:5432/myapp_development
      REDIS_URL: redis://redis:6379/0

  sidekiq:
    build: .
    command: bundle exec sidekiq
    volumes:
      - .:/app
    depends_on:
      - db
      - redis
    environment:
      DATABASE_URL: postgres://app:password@db:5432/myapp_development
      REDIS_URL: redis://redis:6379/0

volumes:
  postgres_data:
  redis_data:
```

## Key Recommendations

### 1. Use a Good Dockerfile for Development

```dockerfile
FROM ruby:3.3-slim

RUN apt-get update -qq && apt-get install -y \
    build-essential \
    libpq-dev \
    nodejs \
    yarn \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Install gems first (better layer caching)
COPY Gemfile Gemfile.lock ./
RUN bundle install

# Copy the rest of the app
COPY . .

EXPOSE 3000
CMD ["bin/rails", "server", "-b", "0.0.0.0"]
```

### 2. Volume Mounts and Performance

On macOS/Windows, bind mounts can be slow. Consider:
- Using `:cached` or `:delegated` on macOS (Docker Desktop).
- Keeping `node_modules` and `tmp` inside the container when possible.
- Using `docker-sync` or Mutagen for very large projects (advanced).

### 3. Database Initialization

Create an entrypoint script:

```bash
#!/bin/bash
set -e

# Wait for Postgres
until pg_isready -h db -U app; do
  echo "Waiting for Postgres..."
  sleep 1
done

bundle exec rake db:prepare

exec "$@"
```

### 4. One-off Commands

```bash
# Rails console
docker compose run --rm web bin/rails console

# Run tests
docker compose run --rm web bundle exec rspec

# Run migrations
docker compose run --rm web bundle exec rake db:migrate
```

### 5. Hot Reloading & File Watching

- Use `bin/rails s` with `listen` gem for file change detection.
- For Stimulus / JS, make sure `node_modules` is properly handled (either mounted or installed inside the container).

### 6. Environment Variables

- Use `.env` files (never commit secrets).
- Keep `DATABASE_URL` and `REDIS_URL` consistent between docker-compose and your local `.env`.

### 7. Healthchecks (Optional but Nice)

```yaml
healthcheck:
  test: ["CMD", "pg_isready", "-U", "app"]
  interval: 10s
  timeout: 5s
  retries: 5
```

## Common Pitfalls

- Permission issues (UID/GID mismatch between host and container)
- Forgetting to rebuild after changing Gemfile or Dockerfile
- Database not being ready when the web process starts (use entrypoint + `depends_on` with `condition: service_healthy`)
- Large image sizes (use multi-stage builds and slim base images)
- Not cleaning up volumes when switching projects

## Production vs Development

Use separate Dockerfiles or multi-stage builds for production. Development images should prioritize speed of iteration and debugging. Production images should be minimal and secure.

---

This reference will be expanded with more advanced patterns (multi-stage production Dockerfile, debugging in containers, using `dip` or `overmind` alongside Docker, etc.) as needed.
