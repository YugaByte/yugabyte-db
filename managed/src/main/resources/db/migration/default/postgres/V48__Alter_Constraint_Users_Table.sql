ALTER TABLE users
	DROP CONSTRAINT ck_users_role,
	ADD CONSTRAINT ck_users_role check (role in ('ReadOnly','Admin','SuperAdmin', 'BackupAdmin'));
