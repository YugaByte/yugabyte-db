ALTER TABLE certificate_info ALTER COLUMN private_key DROP NOT NULL;

ALTER TABLE certificate_info ADD COLUMN cert_type TEXT;

UPDATE certificate_info SET cert_type = 'SelfSigned' WHERE cert_type IS NULL;

ALTER TABLE certificate_info ALTER COLUMN cert_type SET NOT NULL;
