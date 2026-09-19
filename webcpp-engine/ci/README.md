# Local CI/CD

The project has two isolated Docker workflows, both run from the repository root.

```bash
# CI: warning build, all CTest tests, ASan/LSan/UBSan, cppcheck
docker compose -f webcpp-engine/docker-compose.ci.yml run --rm --build ci

# CD verification: builds the frontend, deploys an isolated stack, checks /health,
# then removes its containers and dedicated database volume
webcpp-engine/ci/local-deploy-check.sh
```

The deployment check uses compose project `webcpp-local`, port `127.0.0.1:19443`,
and volume `webcpp_local_mysql_data`; it does not modify the normal `blog` or
`mysql1` containers. The CI image builds the MySQL-enabled application but runs
tests without a database; the deployment workflow additionally validates schema
initialization and the live health endpoint.
