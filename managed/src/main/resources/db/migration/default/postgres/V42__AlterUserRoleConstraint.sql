ALTER TABLE users ALTER COLUMN role type varchar(15);

ALTER TABLE users
	DROP CONSTRAINT ck_user_role,
	ADD CONSTRAINT ck_users_role check (role in ('ReadOnly','Admin','SuperAdmin'));

UPDATE users SET role = 'SuperAdmin' WHERE role = 'Admin';