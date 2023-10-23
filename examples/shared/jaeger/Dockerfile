FROM jaegertracing/all-in-one@sha256:662b3f6e363ecfb84286c0c7258248cb172a5f2a6c4ac962fd73f719d6a5e004
HEALTHCHECK \
    --interval=1s \
    --timeout=1s \
    --start-period=1s \
    --retries=60 \
    CMD wget -q --header='Content-Type:application/json' -O - http://localhost:14269/health | grep "Server available"
