FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app

COPY platform-backend/pyproject.toml /app/pyproject.toml
COPY platform-backend/alembic.ini /app/alembic.ini
COPY platform-backend/app /app/app
RUN pip install --no-cache-dir .

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
