# app/Dockerfile
FROM python:3.11-slim
RUN pip install opentelemetry-api opentelemetry-sdk opentelemetry-exporter-otlp-proto-http
WORKDIR /app
COPY app_service.py .
EXPOSE 8000
CMD ["python3", "app_service.py"]
