-- Close orders stuck in payment_exception whose Alipay trade never existed.
--
-- Context: between 2026-07-29 03:22 and 06:28 UTC every reconciliation failed with
-- alipay_response_signature_invalid, because the running image signed requests without
-- sign_type. No trade was ever created at Alipay for these orders (alipay.trade.query
-- returns ACQ.TRADE_NOT_EXIST), and no payment_notifications rows exist, so no buyer was
-- ever charged. repository.close_unpaid cannot reach them: it only accepts orders still in
-- pending_payment.
--
-- Run inside a transaction and confirm the selected rows before committing.

BEGIN;

CREATE TEMP TABLE stale_attempts ON COMMIT DROP AS
SELECT pa.id AS attempt_id, pa.order_id, pa.out_trade_no, o.amount
FROM payment_attempts pa
JOIN orders o ON o.id = pa.order_id
WHERE o.status = 'payment_exception'
  AND pa.status IN ('created', 'pending')
  AND pa.alipay_trade_no IS NULL
  AND o.paid_at IS NULL
  AND NOT EXISTS (
        SELECT 1 FROM payment_notifications pn WHERE pn.payment_attempt_id = pa.id
  );

SELECT * FROM stale_attempts;

UPDATE payment_attempts SET status = 'closed'
WHERE id IN (SELECT attempt_id FROM stale_attempts);

UPDATE orders SET status = 'closed'
WHERE id IN (SELECT order_id FROM stale_attempts);

INSERT INTO audit_events (id, actor_type, action, subject_type, subject_id, metadata_json)
SELECT gen_random_uuid(), 'system', 'payment.closed', 'order', order_id,
       jsonb_build_object(
         'paymentAttemptId', attempt_id::text,
         'reason', 'alipay_error_acq_trade_not_exist',
         'remediation', 'manual close after sign_type signing defect'
       )
FROM stale_attempts;

COMMIT;
