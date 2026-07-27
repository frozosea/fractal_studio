# Полный аудит покрытия спецификации M0–M7

Дата: 2026-07-25  
Срез: ветка `platform_backend`, HEAD `ad6da1b` до исправлений из этого аудита.  
Источник: `docs/platform-backend-spec.md`, задачи `docs/tasks/00..15`, код `app/`,
миграции, Compose и тесты.

## Итог

Продуктовая логика M1–M6 в основном реализована: есть PostgreSQL-ограничения и
блокировки, транзакционный outbox, private S3, авторитетный Alipay webhook,
ledger и ручные выплаты. Это **не release-ready MVP**: отсутствуют обязательные
сквозные E2E и доказательство реального private Compute v1; есть несколько
реальных контрактных и надёжностных расхождений ниже.

Статусы: **полное** — реализация и локальные доказательства найдены;
**частичное** — основная логика есть, но остался gap; **не доказано** — в этом
репозитории нет требуемого runtime/production evidence.

## Закрытие аудита (2026-07-25)

Все G-01..G-10 закрыты коммитами `3bd80e9` и `2bbf958`, дополненными текущим
изолированным Compose gate. Историческая таблица ниже сохранена как исходное
доказательство, а не как перечень открытых работ.

| Gap | Финальное доказательство закрытия |
|---|---|
| G-01 | `scripts/compute-production-contract.sh` поднимает реальный C++ Compute с service key и выключенным legacy API; `tests/e2e/test_compute_production_contract.py` проверяет auth, replay, conflict, manifest и private artifact. `scripts/e2e-real-compute.sh` дополнительно проводит Platform render→ingest через реальный C++ процесс. |
| G-02 | `scripts/e2e.sh` создаёт чистый профиль `e2e`, bootstrap-ит только допустимые identities и запускает black-box suite. Happy path, recovery, access boundaries и contract matrix прошли: `4 passed`. |
| G-03..G-10 | Lease heartbeat, payout dead-letter recovery, лимит Alipay, lifecycle idempotency, public error namespace, QR decode, redacted logging и `explore.limit=48` реализованы и покрыты unit/HTTP checks. |

| Область | Статус | Краткое основание |
|---|---|---|
| M0 foundation/schema/dev | полное | Compose, миграции и изолированный release-E2E стенд есть. |
| M1 auth/access | полное | Opaque sessions, RBAC, CSRF, rate limits, audit и lifecycle idempotency есть. |
| M2 studio/render | полное | Canonical recipes, quota, mapper, worker и real Compute v1 evidence есть. |
| M3 assets/media | полное условно | Ingest с checksum, private master, derivatives, entitlement download и cleanup реализованы; зависит от Compute и общего outbox gap. |
| M4 marketplace | полное | Listings, immutable versions, search/favorites и `explore.limit=48` реализованы. |
| M5 commerce | полное | Checkout, signed webhook, reconciliation, reversal, entitlement, max Alipay amount и stable errors есть. |
| M6 finance/payout | полное | Ledger/projection, ручная выплата, QR scan и dead-letter cleanup recovery есть. |
| M7 outbox/worker | полное | `SKIP LOCKED`, retry/dead-letter, handlers и lease heartbeat есть. |
| T14 release E2E | полное | Изолированный black-box Compose profile, bootstrap и один runner есть. |
| T15 Compute production | полное | Реальный C++ deployment, direct contract и Platform render/ingest E2E есть. |

## Исходные gaps (закрыты)

| ID / критичность | Расхождение и доказательство | Риск | Минимальное закрытие |
|---|---|---|---|
| G-01 — **blocker** | **T15 не выполнен в этом репозитории.** `docs/tasks/15-compute-production-contract-prerequisite.md` требует развернутый C++ Compute v1, service key, `FSD_ENABLE_LEGACY_API=0` и contract/E2E against real service. В репозитории есть только `app/infrastructure/compute/compute_client.py` и `tests/e2e/compute_stub.py`; Compose указывает на `host.docker.internal:8080`, а не запускает Compute. | Render/ingest production path не подтверждён; несовместимость C++ API, auth или artifact manifest обнаружится после релиза. | Владельцу Compute развернуть private v1, отключить legacy API, провести contract suite и Platform E2E against real service; приложить runbook/evidence. |
| G-02 — **high** | **T14 отсутствует.** Нет `tests/e2e/test_mvp_happy_path.py`, `test_failure_recovery.py`, `test_security_boundaries.py`, `test_api_contract_matrix.py`, `tests/e2e/fixtures`, `scripts/e2e.sh` или Compose `e2e` profile. `tests/conftest.py` пропускает E2E без `E2E_API_URL`; существующие тесты — отдельные opt-in сценарии, часть использует SQL для подготовки/проверки. | Нет одной воспроизводимой проверки всех API, recovery, RBAC, payment и payout путей в чистом окружении. | Добавить isolated profile с Compute/Alipay doubles, bootstrap-only finance/disabled fixture, один runner reset→up→wait→HTTP tests→down, без SQL для business-state setup. |
| G-03 — **high** | **Outbox lease не продлевается.** `app/outbox/worker.py` выдаёт единственный lease, затем синхронно ждёт handler; `app/outbox/repository.py` позволяет другому worker reclaim после `lease_until`. Default lease — 30 s (`app/core/config.py`), тогда как media ffmpeg timeout до 120 s, Compute read до 60 s, а S3 I/O не ограничено тем же lease. | При двух worker один event может исполняться параллельно; domain locks снижают ущерб, но внешние Compute/S3/Alipay операции могут дублироваться. | Heartbeat/renew lease во время dispatch либо гарантированно настроенный lease больше worst-case handler с тестом reclaim во время long handler. |
| G-04 — **high** | **Нет верхней границы суммы Alipay.** Спецификация требует `0.01 ≤ amount ≤ configured ALIPAY_MAX_TOTAL_AMOUNT` (`platform-backend-spec.md`, MVP Technical Limits). В `Settings` нет такого параметра; `ListingCreateInput` и `CheckoutService` проверяют лишь минимум/scale, БД допускает `NUMERIC(18,2)`. | Слишком большая цена попадёт в заказ и лишь затем будет отклонена или поведёт себя непредсказуемо у провайдера. | Добавить decimal setting с безопасным default, validate до listing publish и checkout, DB/check где уместно, unit+HTTP boundary tests. |
| G-05 — **high** | **Payout QR cleanup не восстанавливается из dead-letter.** `PayoutQrCleanupService.cleanup_qr()` only retries via generic outbox; `app/outbox/worker.py` регистрирует handler `payout.qr_cleanup.v1`, но не `register_dead_letter`. После `OUTBOX_MAX_ATTEMPTS` приватный QR остаётся в storage, `qr_deleted_at` не выставляется и нового события не создаётся. Это нарушает lifecycle из M6.1/T13. | Долгое хранение payout QR сверх 30/90 дней — privacy/retention incident. | Добавить domain dead-letter handler: durable retry generation/alert/audit (аналог `AssetCleanupService.dead_letter`), scheduler и тест terminal failure/recovery. |
| G-06 — **medium** | **TTL/lease idempotency — мёртвые поля.** `claim()` пишет `expires_at` и `lease_until`, но `idempotency_repository.lock()` не читает expiry, нет cleanup/reclaim query или scheduler. Completed key replayится вечно; stuck `processing` после истечения lease остаётся `409 idempotency_in_progress`. Нормальный rollback безопасен, но заявленный lifecycle не реализован. | Бесконечный рост таблицы и вечная блокировка ключа после необычного прерывания/неконсистентной записи. | При conflict под lock: удалить/перехватить expired record, reclaim expired processing lease; добавить индексацию/cleanup и tests clock-based expiry/reclaim. |
| G-07 — **medium** | **Публичный error-code контракт не совпадает с T14/spec.** `app/main.py` допускает любой строковый `HTTPException.detail` как `error.code`. Поэтому, например, payout oversize даёт `payout_qr_too_large`, preview rate limit — `preview_rate_limited`, checkout config — `alipay_configuration_incomplete`; T14 требует exact baseline code, включая `payload_too_large`, `quota_exceeded`, `payment_unavailable`. | Frontend и contract tests не могут полагаться на документированный набор кодов; T14 matrix в текущем виде не пройдёт. | Определить namespace: либо нормализовать browser responses к baseline codes и internal reason в safe `details`, либо изменить spec/T14 на документированный расширенный набор; закрепить matrix tests. |
| G-08 — **medium** | **T13 требует QR scan, реализация делает только decode.** `ManualPayoutService.validate_qr_upload()` ограничивает MIME/размер, Pillow `verify/load`, пиксели и пересохраняет PNG, но QR decoder/antimalware scanner отсутствует; не проверяется, что изображение действительно содержит QR. | Некорректный/опасный файл может пройти как «QR», оператор получит невалидное доказательство. | Уточнить policy «scan»; добавить утвержденный malware scanner и/или QR presence decode, fail-closed, timeouts и fixtures. Не пытаться извлекать payee для automatic payout — это явно вне MVP. |
| G-09 — **medium** | **Логирование полагается на дисциплину вызовов.** `log_event()` в `app/core/logging.py` без redaction/deny-list конкатенирует все переданные поля. Спецификация запрещает secrets, cookies, signed URLs, S3 keys, QR и raw Alipay form в message. В проверенных текущих вызовах явной утечки не найдено, но технической защиты и regression test нет. | Будущая ошибка вызова может записать секрет в production logs. | Remove arbitrary fields from message либо redact known keys/patterns и тестировать запретные значения; сохранить structured correlation fields отдельно. |
| G-10 — **low** | **`GET /v1/explore` принимает `limit=1..100`, а спецификация — `1..48`.** Доказательство: `app/marketplace/router.py`; exact requirement — Public API section. | Клиент получает недокументированную нагрузку/страницы; расхождение с OpenAPI/E2E matrix. | Поменять `le=48`, обновить/добавить HTTP test на 49→422. |

## Что специально не признано gap

- Soft-deleted sold asset остаётся downloadable entitled buyer: это соответствует M3
  («sold master retained»), а не утечка доступа.
- Preview использует явный 5 s timeout в `PreviewService`; общий Compute timeout 60 s
  относится к durable calls.
- Normal failed mutation не кешируется idempotency как success: claim и business write
  находятся в одном DB transaction, rollback снимает claim.
- Manual payout намеренно не извлекает payee из QR и не использует Alipay transfer API;
  это прямо deferred after MVP. G-08 касается только заявленного validation/scan файла.
- `M1_M2.md` — historical closure record. Он не покрывает G-01..G-10 и не должен
  интерпретироваться как актуальный release sign-off.

## Реализованное и проверенное покрытие

- **M1:** opaque hashed sessions, role lookup из DB на каждый protected request,
  session rotation/revocation, CSRF/origin checks, scrypt password hashes и Redis
  fail-closed login/register limits (`app/auth/`, `app/core/access_middleware.py`).
- **M2/M3:** immutable canonical recipes; bounded preview; durable quota reservation;
  versioned private Compute client with service Bearer key; artifact allowlist,
  checksum/size verification, private master upload, derivative worker and protected
  owner/entitlement signed URLs (`app/studio/`, `app/assets/`).
- **M4:** draft/publish state, immutable listing versions/licence offer snapshots,
  public discovery and favorites (`app/marketplace/`).
- **M5/M6:** frozen order snapshots, RSA2 verification, duplicate webhook fingerprint,
  reconciliation/reversal, append-only ledger trigger, creator balance projection,
  operator-only payout settlement (`app/commerce/`, `app/finance/`, migrations `0010+`).
- **M7:** unique transactional events, `FOR UPDATE SKIP LOCKED`, retry count separate
  from poll deferral, dead-letter handling for render/payment/asset cleanup
  (`app/outbox/`, `app/studio/render_worker.py`, `app/assets/cleanup_service.py`).
- **Production configuration:** secure cookie/secret/HTTPS/CORS, S3 encryption and
  Alipay production fields are fail-closed in `Settings`. Compute production deployment
  checks remain G-01.

## Проверка, выполненная для аудита

1. Статический проход по всем роутам, service/repository/worker, migrations и
   `docker-compose.dev.yml`.
2. Сопоставление public API, state machines, transaction rules, limits, M1–M7 и
   T14/T15 acceptance criteria со строками кода и тестовой инвентаризацией.
3. Не выполнено как доказательство release: real Compute deployment и полный
   black-box E2E отсутствуют (G-01/G-02), поэтому их нельзя заменить локальным
   unit test или Compute stub.

## Приоритет закрытия

1. G-01 и G-02 — release blockers: real Compute contract и один reproducible E2E gate.
2. G-03 и G-05 — исключить дублирование external work и retention leak.
3. G-04 — финансовый лимит до production checkout.
4. G-06–G-10 — контракт, observability и hygiene; закрыть до публичного API freeze.
