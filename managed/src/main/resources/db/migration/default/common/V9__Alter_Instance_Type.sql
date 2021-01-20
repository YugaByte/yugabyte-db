alter table instance_type drop constraint ck_instance_type_volume_type;

alter table instance_type drop column volume_count;
alter table instance_type drop column volume_size_gb;
alter table instance_type drop column volume_type;
