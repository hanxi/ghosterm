FROM ubuntu:22.04@sha256:4f838adc7181d9039ac795a7d0aba05a9bd9ecd480d294483169c5def983b64d
ENV DEBIAN_FRONTEND=noninteractive TMPDIR=/fixture/tmp
RUN mkdir -p /fixture/tmp && apt-get update && apt-get install -y --no-install-recommends openssh-server \
    && rm -rf /var/lib/apt/lists/* && useradd --create-home --shell /bin/sh fixture \
    && mkdir -p /home/fixture/.ssh && chmod 700 /home/fixture/.ssh \
    && chown fixture:fixture /home/fixture/.ssh
COPY sshd_config /fixture/sshd_config
COPY start.sh /fixture/start.sh
ENTRYPOINT ["/bin/sh", "/fixture/start.sh"]
